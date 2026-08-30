#include "core/kodi_client.h"

#include "core/notification.h"
#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/websocket_client.h"
#include "platform/mdns_browser.h"
#include "third_party/nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using homedeck::KodiClient;
using homedeck::KodiConnectionState;
using homedeck::KodiPlaybackState;
using homedeck::MdnsService;

// Returns the discovered-instance list a test scripts; records how many
// times the connection loop browsed. KodiClient makes one Browse() call
// per (re)connect attempt (see ConnectionLoop()).
class FakeMdnsBrowser : public homedeck::MdnsBrowser {
public:
    std::vector<MdnsService> Browse(const std::string& service_type, std::chrono::milliseconds) override {
        std::lock_guard<std::mutex> lock(mutex_);
        last_service_type_ = service_type;
        browse_count_++;
        return instances_;
    }

    void SetInstances(std::vector<MdnsService> instances) {
        std::lock_guard<std::mutex> lock(mutex_);
        instances_ = std::move(instances);
    }

    int BrowseCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return browse_count_;
    }

    std::string LastServiceType() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_service_type_;
    }

private:
    std::mutex mutex_;
    std::vector<MdnsService> instances_;
    int browse_count_ = 0;
    std::string last_service_type_;
};

// Shared control block for the per-connect FakeWebSocketClient instances
// KodiClient's factory hands out (same pattern as
// harmony_connection_test.cpp's WsScript). Auto-answers JSON-RPC
// requests: SendText() parses the request's `id`/`method`, looks up a
// scripted `result` value for that method, and queues
// {"jsonrpc":"2.0","id":<id>,"result":<result>} for the next blocking
// ReceiveText(). `pushed` frames model Kodi's unsolicited
// notifications - delivered ahead of any correlated reply, and the only
// thing a 0ms ReceiveText() (PumpNotifications()) ever sees.
struct WsScript {
    std::mutex mutex;
    bool connect_ok = true;
    bool send_ok = true;
    bool dead = false;  // once set, every ReceiveText() returns nullopt (transport gone)
    std::vector<std::string> connect_urls;
    std::vector<std::string> sent;
    std::map<std::string, std::string> results;  // method -> raw JSON for the "result" value
    std::deque<std::string> pushed;
    std::deque<std::string> ready;
    int close_count = 0;
};

class FakeWebSocketClient : public homedeck::WebSocketClient {
public:
    explicit FakeWebSocketClient(std::shared_ptr<WsScript> script) : script_(std::move(script)) {}

    bool Connect(const std::string& url) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->connect_urls.push_back(url);
        return script_->connect_ok;
    }

    bool SendText(const std::string& text) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->sent.push_back(text);
        if (!script_->send_ok) {
            return false;
        }
        nlohmann::json request = nlohmann::json::parse(text, nullptr, false);
        if (request.is_object() && request.contains("id") && request.contains("method")) {
            const std::string method = request["method"].get<std::string>();
            auto it = script_->results.find(method);
            if (it != script_->results.end()) {
                nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
                reply["result"] = nlohmann::json::parse(it->second, nullptr, false);
                script_->ready.push_back(reply.dump());
            }
        }
        return true;
    }

    std::optional<std::string> ReceiveText(int timeout_ms) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        if (script_->dead) {
            return std::nullopt;
        }
        if (!script_->pushed.empty()) {
            std::string frame = std::move(script_->pushed.front());
            script_->pushed.pop_front();
            return frame;
        }
        if (timeout_ms != 0 && !script_->ready.empty()) {
            std::string frame = std::move(script_->ready.front());
            script_->ready.pop_front();
            return frame;
        }
        return std::nullopt;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->close_count++;
    }

private:
    std::shared_ptr<WsScript> script_;
};

void Push(const std::shared_ptr<WsScript>& script, std::string frame) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->pushed.push_back(std::move(frame));
}

int CountSent(const std::shared_ptr<WsScript>& script, const std::string& needle) {
    std::lock_guard<std::mutex> lock(script->mutex);
    int n = 0;
    for (const std::string& s : script->sent) {
        if (s.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

// True once a single sent frame contains every needle (e.g. the method
// and a specific param).
bool SentFrameHasAll(const std::shared_ptr<WsScript>& script, std::initializer_list<std::string> needles) {
    std::lock_guard<std::mutex> lock(script->mutex);
    for (const std::string& s : script->sent) {
        bool all = true;
        for (const std::string& n : needles) {
            if (s.find(n) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) {
            return true;
        }
    }
    return false;
}

// A play-state notification triggers an immediate reconcile poll (to
// pick up position/duration - see needs_immediate_poll_), and that
// poll's Player.GetProperties reply carries `speed` too. Kodi returns
// the current speed there; the static fake needs it kept in sync with
// whatever the test just pushed, or the poll overwrites the
// notification's play-state with a stale one.
void SetPolledSpeed(const std::shared_ptr<WsScript>& script, int speed) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->results["Player.GetProperties"] =
        R"({"speed":)" + std::to_string(speed) +
        R"(,"percentage":25.0,"time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
        R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0}})";
}

// Scripts a plausible "nothing playing" reconcile: app props plus an
// empty active-players list.
void ScriptIdleKodi(const std::shared_ptr<WsScript>& script) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->results["Application.GetProperties"] =
        R"({"volume":42,"muted":false,"version":{"major":21,"minor":2}})";
    script->results["Player.GetActivePlayers"] = R"([])";
}

// Scripts a "playing an episode" reconcile: active player 1, transport
// position, and a library item.
void ScriptPlayingKodi(const std::shared_ptr<WsScript>& script) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->results["Application.GetProperties"] =
        R"({"volume":42,"muted":false,"version":{"major":21,"minor":2}})";
    script->results["Player.GetActivePlayers"] = R"([{"playerid":1,"playertype":"internal","type":"video"}])";
    script->results["Player.GetProperties"] =
        R"({"speed":1,"percentage":25.0,"time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
        R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0}})";
    script->results["Player.GetItem"] =
        R"({"item":{"title":"","label":"Some Show","showtitle":"Some Show","season":3,"episode":7,"type":"episode"}})";
}

MdnsService Instance(const std::string& name, const std::string& address, const std::string& uuid, uint16_t port = 9090) {
    MdnsService svc;
    svc.instance_name = name;
    svc.address = address;
    svc.port = port;
    if (!uuid.empty()) {
        svc.txt["uuid"] = uuid;
    }
    return svc;
}

template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 400) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

constexpr std::chrono::milliseconds kFastBackoff{20};
constexpr std::chrono::milliseconds kFastReconcile{40};
constexpr std::chrono::milliseconds kFastPump{10};
constexpr std::chrono::milliseconds kFastBrowse{5};

class KodiClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_kodi_client_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
    }
    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    // reconcile defaults to a fast interval; notification-focused tests
    // pass a long one so only ConnectAndPrime()'s single up-front poll
    // runs and pushed notifications are the sole thing driving state
    // (against a static script the poll and a notification otherwise
    // disagree - Kodi's own poll reads live state, so they agree).
    std::unique_ptr<KodiClient> MakeClient(std::shared_ptr<WsScript> script, FakeMdnsBrowser& browser,
                                           homedeck::Storage& storage, homedeck::EventBus& bus,
                                           std::chrono::milliseconds reconcile = kFastReconcile,
                                           std::chrono::milliseconds max_command_age = std::chrono::seconds(5)) {
        return std::make_unique<KodiClient>(
            [script] { return std::make_unique<FakeWebSocketClient>(script); }, browser, storage, bus, kFastBackoff,
            kFastBackoff, reconcile, kFastPump, kFastBrowse, max_command_age);
    }

    static constexpr std::chrono::seconds kNoReconcile{30};

    std::filesystem::path root_dir_;
};

// --- IsValidKodiHost -------------------------------------------------------

TEST(IsValidKodiHostTest, AcceptsAPlainHostnameOrIpAndEmpty) {
    EXPECT_TRUE(homedeck::IsValidKodiHost("10.0.30.20"));
    EXPECT_TRUE(homedeck::IsValidKodiHost("kodi.local"));
    EXPECT_TRUE(homedeck::IsValidKodiHost("")) << "empty means 'use discovery', not malformed";
}

TEST(IsValidKodiHostTest, RejectsSchemeWhitespacePathStructuralCharsAndBareIpv6) {
    EXPECT_FALSE(homedeck::IsValidKodiHost("ws://10.0.30.20"));
    EXPECT_FALSE(homedeck::IsValidKodiHost("10.0.30.20 "));
    EXPECT_FALSE(homedeck::IsValidKodiHost("10.0.30.20/jsonrpc"));
    EXPECT_FALSE(homedeck::IsValidKodiHost("host#frag"));
    EXPECT_FALSE(homedeck::IsValidKodiHost("host?q"));
    EXPECT_FALSE(homedeck::IsValidKodiHost("user@host"));
    EXPECT_FALSE(homedeck::IsValidKodiHost("fe80::1"));
}

// --- Discovery / instance selection --------------------------------------

TEST_F(KodiClientTest, ManualHostOverrideConnectsWithoutDiscovery) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        ASSERT_FALSE(script->connect_urls.empty());
        EXPECT_EQ(script->connect_urls.front(), "ws://10.0.30.20:9090/jsonrpc");
    }
    EXPECT_EQ(browser.BrowseCount(), 0) << "an explicit host must skip discovery entirely";
    EXPECT_EQ(client->Snapshot().volume, 42);
    client->Stop();
}

TEST_F(KodiClientTest, SingleDiscoveredInstanceIsAutoSelected) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Shield", "10.0.30.20", "uuid-a")});
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    EXPECT_EQ(browser.LastServiceType(), "_xbmc-jsonrpc._tcp");
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->connect_urls.front(), "ws://10.0.30.20:9090/jsonrpc");
    }
    client->Stop();
}

TEST_F(KodiClientTest, SavedUuidSelectsTheMatchingInstanceAmongSeveral) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kInstanceUuidKey, 1, "uuid-b"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Living Room", "10.0.30.20", "uuid-a"),
                          Instance("Bedroom", "10.0.30.21", "uuid-b")});
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->connect_urls.front(), "ws://10.0.30.21:9090/jsonrpc");
    }
    client->Stop();
}

TEST_F(KodiClientTest, MultipleInstancesWithNoSelectionStaysDisconnected) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Living Room", "10.0.30.20", "uuid-a"),
                          Instance("Bedroom", "10.0.30.21", "uuid-b")});
    auto script = std::make_shared<WsScript>();

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);
    EXPECT_EQ(client->Snapshot().discovered.size(), 2u);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty()) << "ambiguous discovery must not guess an instance";
    }
    client->Stop();
}

TEST_F(KodiClientTest, SavedUuidOfflineDoesNotFallBackToAnotherInstance) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kInstanceUuidKey, 1, "uuid-gone"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Some Other Box", "10.0.30.99", "uuid-other")});
    auto script = std::make_shared<WsScript>();

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty())
            << "the selected instance being offline must not silently control a different room";
    }
    client->Stop();
}

// A discovered instance's host is concatenated straight into a ws://
// URL without IsValidKodiHost() (it never came from the user). A hostile
// mDNS responder advertising a host with userinfo / a path / whitespace
// must be dropped, not connected to.
TEST_F(KodiClientTest, DiscoveredInstanceWithAnUnsafeHostIsIgnored) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Evil", "attacker@10.0.0.9", "uuid-evil")});
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);
    EXPECT_TRUE(client->Snapshot().discovered.empty()) << "an unsafe host must not even reach the pick-one list";
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty());
    }
    client->Stop();
}

// --- "Not reachable" is not a fault ------------------------------------------

TEST_F(KodiClientTest, ConnectFailureEntersErrorWithoutPublishingANotification) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    std::atomic<int> notifications{0};
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = false;
    }

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kError; }));
    // Let it churn through several failed retries. Per ADR-0030, an
    // unreachable Kodi is a normal state on Android/Google TV, not a
    // fault - so no NotificationEvent, unlike Harmony's kError path.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    EXPECT_EQ(notifications.load(), 0) << "an unreachable Kodi must not raise a notification";
    client->Stop();
}

// --- Pushed notifications --------------------------------------------------

TEST_F(KodiClientTest, PlayNotificationPopulatesNowPlayingIdentityAndState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    std::atomic<int> now_playing_events{0};
    auto sub = bus.Subscribe<homedeck::KodiNowPlayingChangedEvent>(
        [&now_playing_events](const homedeck::KodiNowPlayingChangedEvent&) { now_playing_events++; });

    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPlay","params":{"data":{"item":{"title":"An Ep",)"
                 R"("showtitle":"The Show","season":10,"episode":7,"type":"episode"},"player":{"playerid":-1,)"
                 R"("speed":1}}}})");

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.title == "An Ep"; }));
    auto np = client->Snapshot().now_playing;
    EXPECT_EQ(np.show_title, "The Show");
    EXPECT_EQ(np.season, 10);
    EXPECT_EQ(np.episode, 7);
    EXPECT_EQ(np.media_type, "episode");
    EXPECT_EQ(np.playback, KodiPlaybackState::kPlaying);
    EXPECT_GT(now_playing_events.load(), 0);
    client->Stop();
}

// Per kodi.md's "Identity vs. timing" (ADR-0030 records the underlying
// protocol facts): once a Player.On* notification supplies identity, a
// later reconcile poll's Player.GetItem must not overwrite it - the
// main reason is add-on playback, where GetItem
// returns blanks that ApplyItemFields() would already leave alone, so
// this scripts the poll returning a *different, non-blank* title
// instead - the only way to prove identity_from_notification_ actually
// suppresses the overwrite rather than ApplyItemFields' own
// leave-blank-fields-alone behavior doing it incidentally.
TEST_F(KodiClientTest, NotificationIdentitySurvivesALaterPollWithADifferentGetItemResult) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);  // GetItem starts as "Some Show" S3E7 (no notification seen yet)

    auto client = MakeClient(script, browser, storage, bus);  // default (fast) reconcile interval
    client->Start();
    // ConnectAndPrime's up-front poll has no notification to prefer, so
    // GetItem's own value is used - confirms the starting state this
    // test's premise depends on.
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.title == "Some Show"; }));

    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnAVChange","params":{"data":{"item":{"title":"",)"
                 R"("label":"Add-on Movie","type":"unknown"},"player":{"playerid":1,"speed":1}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.title == "Add-on Movie"; }));

    // The next periodic poll's GetItem now returns a different,
    // non-blank title - if identity_from_notification_ didn't suppress
    // it, ApplyItemFields() would happily overwrite with this.
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Player.GetItem"] = R"({"item":{"title":"Wrong Title From Poll","type":"movie"}})";
    }
    std::this_thread::sleep_for(kFastReconcile * 3);
    EXPECT_EQ(client->Snapshot().now_playing.title, "Add-on Movie")
        << "the notification's identity must survive a later poll's differing GetItem result";
    client->Stop();
}

TEST_F(KodiClientTest, PauseThenStopNotificationsTrackPlaybackState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    SetPolledSpeed(script, 1);
    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPlay","params":{"data":{"item":{"title":"X","type":"movie"},)"
                 R"("player":{"playerid":1,"speed":1}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.playback == KodiPlaybackState::kPlaying; }));

    SetPolledSpeed(script, 0);
    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPause","params":{"data":{"item":{"title":"X","type":"movie"},)"
                 R"("player":{"playerid":1,"speed":0}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.playback == KodiPlaybackState::kPaused; }));

    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnStop","params":{"data":{"end":false,"item":{"title":"X"}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.playback == KodiPlaybackState::kInactive; }));
    EXPECT_TRUE(client->Snapshot().now_playing.title.empty());
    client->Stop();
}

TEST_F(KodiClientTest, VolumeChangedNotificationUpdatesTheSnapshot) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    Push(script, R"({"jsonrpc":"2.0","method":"Application.OnVolumeChanged","params":{"data":{"volume":11,)"
                 R"("muted":true}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().volume == 11; }));
    EXPECT_TRUE(client->Snapshot().muted);
    client->Stop();
}

// A reconcile poll that finds nothing changed (connected, idle, same
// volume/mute) must not publish KodiNowPlayingChangedEvent - otherwise
// every widget/screen bound to it re-renders once per reconcile interval
// forever while idle.
TEST_F(KodiClientTest, IdleReconcilePollsDoNotRepublishTheNowPlayingEvent) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    std::atomic<int> now_playing_events{0};
    auto sub = bus.Subscribe<homedeck::KodiNowPlayingChangedEvent>(
        [&now_playing_events](const homedeck::KodiNowPlayingChangedEvent&) { now_playing_events++; });

    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus, kFastReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    // kFastReconcile is 40ms - let a good number of poll cycles run.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_LE(now_playing_events.load(), 1) << "idle reconcile cycles should not each fire an event";
    client->Stop();
}

TEST_F(KodiClientTest, ReconcilePollFillsPositionAndDurationFromPolledProperties) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    // 5 min position / 20 min total, from ScriptPlayingKodi's GetProperties.
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.duration_ms == 20 * 60 * 1000; }));
    auto np = client->Snapshot().now_playing;
    EXPECT_EQ(np.position_ms, 5 * 60 * 1000);
    EXPECT_DOUBLE_EQ(np.percent, 25.0);
    // GetItem's `title` is blank for this (add-on-style) item; `label` is
    // used instead - ADR-0030's merge rule.
    EXPECT_EQ(np.title, "Some Show");
    EXPECT_EQ(np.season, 3);
    client->Stop();
}

// canseek comes only from the reconcile poll's Player.GetProperties (no
// Player.On* notification carries it), and gates NowPlayingScreen's seek
// buttons. Prove the poll actually threads it onto the snapshot for both
// values.
TEST_F(KodiClientTest, ReconcilePollCarriesCanSeekFromPlayerProperties) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Player.GetProperties"] =
            R"({"speed":1,"percentage":25.0,"time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
            R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0},"canseek":true})";
    }

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.can_seek; }))
        << "a seekable source must report can_seek true after a reconcile poll";

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Player.GetProperties"] =
            R"({"speed":1,"percentage":25.0,"time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
            R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0},"canseek":false})";
    }
    ASSERT_TRUE(WaitFor([&] { return !client->Snapshot().now_playing.can_seek; }))
        << "a later poll seeing canseek:false must clear it again";
    client->Stop();
}

TEST_F(KodiClientTest, InterleavedNotificationDuringAPollIsHandledAndThePollStillCompletes) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);
    // A notification is already buffered when the first reconcile poll's
    // Call() starts reading: Call() must dispatch it and keep waiting
    // for its own id-matched reply, not return the notification frame as
    // if it were the response.
    Push(script, R"({"jsonrpc":"2.0","method":"Application.OnVolumeChanged","params":{"data":{"volume":7}}})");

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();

    // Reaching kConnected at all means every one of ConnectAndPrime()'s
    // Call()s correlated its reply past the stray notification.
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }))
        << "the id-correlation loop must not mistake a notification for the reply";
    // The GetProperties reply (20 min total) was still delivered to the
    // poll, not lost.
    EXPECT_EQ(client->Snapshot().now_playing.duration_ms, 20 * 60 * 1000);
    // The stray notification was consumed as a notification (volume 7),
    // then the poll's own Application.GetProperties reply (volume 42)
    // landed as the authoritative value.
    EXPECT_EQ(client->Snapshot().volume, 42);
    client->Stop();
}

TEST_F(KodiClientTest, TransportDeathMidSessionReconnects) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->dead = true;  // every ReceiveText() now fails -> next reconcile poll sees the drop
    }
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->connect_urls.size() >= 2;  // it tried to reconnect
    }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->dead = false;  // let it recover
    }
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->Stop();
}

TEST_F(KodiClientTest, TriggerReconnectReResolvesTheTarget) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;  // nothing discovered yet
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);

    // Configure a host, then poke the loop instead of waiting out the recheck.
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));
    client->TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->Stop();
}

// --- Commands (Phase C) -------------------------------------------------

// Connects to a manual host that is playing an episode (so
// ResolveActivePlayerId() returns 1), reconcile slowed so command sends
// are the only traffic the assertions look at.
#define KODI_COMMAND_RIG()                                                                   \
    homedeck::HostSettingsStore settings_store(root_dir_);                                    \
    homedeck::HostCacheStore cache_store(root_dir_);                                          \
    homedeck::HostSecretStore secret_store(root_dir_);                                        \
    homedeck::Storage storage(settings_store, cache_store, secret_store);                     \
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20")); \
    homedeck::EventBus bus;                                                                   \
    FakeMdnsBrowser browser;                                                                  \
    auto script = std::make_shared<WsScript>();                                               \
    ScriptPlayingKodi(script)

TEST_F(KodiClientTest, PlaybackCommandsSendTheRightMethodWithTheResolvedPlayerId) {
    KODI_COMMAND_RIG();
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->PlayPause();
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.PlayPause", "\"playerid\":1"}); }));

    client->StopPlayback();
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.Stop", "\"playerid\":1"}); }));

    client->SeekPercent(66);
    ASSERT_TRUE(
        WaitFor([&] { return SentFrameHasAll(script, {"Player.Seek", "\"playerid\":1", "\"percentage\":66"}); }));

    client->SetSpeed(4);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.SetSpeed", "\"speed\":4"}); }));
    client->Stop();
}

TEST_F(KodiClientTest, GlobalCommandsNeedNoPlayerId) {
    KODI_COMMAND_RIG();
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->SetVolume(30);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Application.SetVolume", "\"volume\":30"}); }));

    client->ToggleMute();
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Application.SetMute", "\"mute\":\"toggle\""}); }));

    client->SendInput(homedeck::KodiInput::kUp);
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "\"method\":\"Input.Up\"") == 1; }));

    client->SendInput(homedeck::KodiInput::kShowOsd);
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "Input.ShowOSD") == 1; }));

    client->OpenLibraryItem("movieid", 42, /*resume=*/true);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.Open", "\"movieid\":42", "\"resume\":true"}); }));
    client->Stop();
}

TEST_F(KodiClientTest, ACommandReplyFrameDoesNotDisturbTheSnapshot) {
    KODI_COMMAND_RIG();
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    int volume_before = client->Snapshot().volume;

    // The fake auto-queues {"id":N,"result":...} for every sent command;
    // PumpNotifications() must discard it (no "method" field), not treat
    // it as state.
    client->PlayPause();
    client->SendInput(homedeck::KodiInput::kSelect);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    EXPECT_EQ(client->Snapshot().volume, volume_before);
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    client->Stop();
}

TEST_F(KodiClientTest, ACommandQueuedWhileDisconnectedIsSentOnceConnected) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = false;
    }

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kError; }));

    client->SendInput(homedeck::KodiInput::kSelect);  // queued against a dead connection
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(CountSent(script, "Input.Select"), 0);

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = true;
    }
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "Input.Select") == 1; }))
        << "the queued command must go out once a connection exists";
    client->Stop();
}

TEST_F(KodiClientTest, StaleNonExemptCommandsAreDroppedButStopIsKept) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = false;
    }

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile, /*max_command_age=*/std::chrono::milliseconds(30));
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kError; }));

    client->SendInput(homedeck::KodiInput::kSelect);  // not exempt
    client->StopPlayback();                            // keep_when_stale
    std::this_thread::sleep_for(std::chrono::milliseconds(90));  // both now older than max_command_age

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = true;
    }
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.Stop", "\"playerid\":1"}); }))
        << "a stop stays worth attempting however late";
    EXPECT_EQ(CountSent(script, "Input.Select"), 0) << "the stale navigation command was dropped";
    client->Stop();
}

// The pending-command queue is capped at KodiClient::kMaxPendingCommands
// (20). Enqueue past that while no target can be resolved - the loop
// sits in its no-target wait, which doesn't watch pending_commands_, so
// every enqueue lands before any drain - then confirm the oldest were
// dropped and the newest survived once a host is configured. Mirrors
// HarmonyConnection's EnqueueingPastTheCapDropsTheOldestEntriesFirst.
TEST_F(KodiClientTest, PendingQueueDropsTheOldestCommandsPastItsCap) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // No host set, no instances discovered: ResolveTarget() returns
    // nullopt and the loop waits in kDisconnected without watching
    // commands.

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kDisconnected; }));

    constexpr int kEnqueued = 25;
    constexpr int kCap = 20;
    for (int volume = 0; volume < kEnqueued; ++volume) {
        client->SetVolume(volume);  // global command; distinct "volume":N payload each
    }

    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));
    client->TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "Application.SetVolume") >= kCap; }));

    // Anything above the cap would have been sent in the same drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(CountSent(script, "Application.SetVolume"), kCap);
    for (int volume = 0; volume < kEnqueued - kCap; ++volume) {
        EXPECT_EQ(CountSent(script, "\"volume\":" + std::to_string(volume) + "}"), 0)
            << "dropped volume " << volume << " should never have been sent";
    }
    EXPECT_EQ(CountSent(script, "\"volume\":24}"), 1) << "the newest command must survive";
    client->Stop();
}

// PumpNotifications() bounds its own non-blocking drain at
// kMaxPumpIterations (32, kodi_client.cpp) so a peer that keeps the
// receive buffer full can't turn one pump into an unbounded loop. Queue
// more than the cap as one atomic batch after connecting (so
// ConnectAndPrime()'s own Call()s don't consume it first), with a long
// pump interval so the first drain's stopping point is observable
// before the next cycle. Mirrors HarmonyConnection's
// DrainStaleMessagesStopsAtItsOwnIterationCap.
TEST_F(KodiClientTest, PumpNotificationDrainStopsAtItsIterationCap) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    // Constructed directly rather than via MakeClient(): a 200 ms pump
    // interval (vs. the shared 10 ms) leaves a clear window between the
    // first drain and the next to check where it stopped.
    KodiClient client([script] { return std::make_unique<FakeWebSocketClient>(script); }, browser, storage, bus,
                      kFastBackoff, kFastBackoff, /*reconcile_interval=*/std::chrono::seconds(30),
                      /*pump_interval=*/std::chrono::milliseconds(200), kFastBrowse,
                      /*max_pending_command_age=*/std::chrono::seconds(5));
    client.Start();
    ASSERT_TRUE(WaitFor([&] { return client.Snapshot().state == KodiConnectionState::kConnected; }));

    constexpr int kQueued = 40;             // > kMaxPumpIterations (32)
    constexpr size_t kLeftAfterOneDrain = 8;  // kQueued - 32
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        for (int v = 1; v <= kQueued; ++v) {
            script->pushed.push_back(
                R"({"jsonrpc":"2.0","method":"Application.OnVolumeChanged","params":{"data":{"volume":)" +
                std::to_string(v) + R"(}}})");
        }
    }

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->pushed.size() <= kLeftAfterOneDrain;
    }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->pushed.size(), kLeftAfterOneDrain)
            << "one pump must stop at kMaxPumpIterations, not drain the whole backlog";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->pushed.size(), kLeftAfterOneDrain) << "and must not resume before the next pump cycle";
    }
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->pushed.empty();
    }));
    client.Stop();
}

// --- HostWebSocketClient over a loopback JSON-RPC peer -------------------

// This fake Kodi binds an ephemeral port and the test advertises it
// through FakeMdnsBrowser, so ResolveTarget()'s discovery path carries
// the real port into WebSocketUrl() (only the port-less manual-override
// path assumes 9090). Binding 9090 itself would collide with a Kodi
// actually running on the developer's machine - the exact setup anyone
// working on this module has.

std::string ReadHttpRequest(int fd) {
    std::string data;
    char buf[2048];
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    return data;
}

std::string HeaderValue(const std::string& request, const std::string& name) {
    size_t pos = request.find(name + ": ");
    if (pos == std::string::npos) return "";
    pos += name.size() + 2;
    return request.substr(pos, request.find("\r\n", pos) - pos);
}

std::string WsAcceptKey(const std::string& client_key) {
    std::string combined = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), digest);
    unsigned char encoded[64];
    size_t out_len = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len, digest, sizeof(digest));
    return std::string(reinterpret_cast<char*>(encoded), out_len);
}

std::string ReadClientFrame(int fd) {
    unsigned char header[2];
    if (read(fd, header, 2) != 2) return "";
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (read(fd, ext, 2) != 2) return "";
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    }
    unsigned char mask[4] = {};
    if (masked && read(fd, mask, 4) != 4) return "";
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, &payload[got], len - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
    }
    return payload;
}

void SendServerFrame(int fd, const std::string& payload) {
    std::vector<unsigned char> header;
    header.push_back(0x81);
    if (payload.size() < 126) {
        header.push_back(static_cast<unsigned char>(payload.size()));
    } else {
        header.push_back(126);
        header.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
        header.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
    }
    send(fd, header.data(), header.size(), MSG_NOSIGNAL);
    send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
}

// Binds a kernel-assigned loopback port and reports it via *out_port.
// Returns -1 (not a half-open fd) on any failure so the caller can
// assert rather than accept() on a socket that never listened.
int ListenLoopback(uint16_t* out_port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // kernel picks a free ephemeral port
    socklen_t addr_len = sizeof(addr);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 2) != 0 ||
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        close(listen_fd);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);
    return listen_fd;
}

// Answers the JSON-RPC methods ReconcilePoll() issues, then pushes one
// unsolicited notification - enough to prove KodiClient drives the
// libcurl-backed HostWebSocketClient (id correlation across genuine
// frames, a 0ms drain that actually sees a buffered push) end to end.
// Exercises the gap
// [[feedback-fake-doubles-hide-backend-timing-bugs]] names.
void RunFakeKodi(int listen_fd, std::atomic<bool>& stop) {
    // Bounded accept() so a test that fails before the client ever
    // connects still lets this thread observe `stop` and exit, rather
    // than blocking the join() forever.
    timeval accept_timeout{.tv_sec = 0, .tv_usec = 100 * 1000};
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));
    int fd = -1;
    while (!stop.load()) {
        fd = accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return;
    }
    if (fd < 0) return;
    std::string upgrade = ReadHttpRequest(fd);
    std::string key = HeaderValue(upgrade, "Sec-WebSocket-Key");
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           WsAcceptKey(key) + "\r\n\r\n";
    send(fd, response.data(), response.size(), MSG_NOSIGNAL);

    bool pushed_once = false;
    while (!stop.load()) {
        std::string frame = ReadClientFrame(fd);
        if (frame.empty()) break;
        nlohmann::json request = nlohmann::json::parse(frame, nullptr, false);
        if (!request.is_object() || !request.contains("id")) continue;
        const std::string method = request.value("method", "");
        nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
        if (method == "Application.GetProperties") {
            reply["result"] = {{"volume", 55}, {"muted", false}, {"version", {{"major", 21}, {"minor", 2}}}};
        } else if (method == "Player.GetActivePlayers") {
            reply["result"] = nlohmann::json::array();
        } else {
            reply["result"] = "OK";
        }
        SendServerFrame(fd, reply.dump());

        if (!pushed_once) {
            pushed_once = true;
            SendServerFrame(fd, R"({"jsonrpc":"2.0","method":"Application.OnVolumeChanged",)"
                                R"("params":{"data":{"volume":3,"muted":true}}})");
        }
    }
    close(fd);
}

TEST_F(KodiClientTest, RealBackendConnectsReconcilesAndHandlesAPushedNotification) {
    uint16_t port = 0;
    int listen_fd = ListenLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::atomic<bool> stop_server{false};
    // Joins the fake-server thread however this scope exits - a bare
    // std::thread left joinable by an early ASSERT failure would
    // std::terminate() the whole test binary. Declared before `client`
    // so `client` (and the socket it holds) tears down first, unblocking
    // RunFakeKodi's read loop before the join.
    struct ServerGuard {
        std::thread thread;
        std::atomic<bool>& stop;
        int listen_fd;
        ~ServerGuard() {
            stop.store(true);
            ::shutdown(listen_fd, SHUT_RDWR);
            if (thread.joinable()) thread.join();
            close(listen_fd);
        }
    } guard{std::thread([listen_fd, &stop_server] { RunFakeKodi(listen_fd, stop_server); }), stop_server, listen_fd};

    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    // Discovery, not a manual host override: that carries the ephemeral
    // port through to WebSocketUrl(), and a single instance is
    // auto-selected.
    browser.SetInstances({Instance("Loopback", "127.0.0.1", "uuid-loopback", port)});

    KodiClient client([] { return std::make_unique<homedeck::HostWebSocketClient>(); }, browser, storage, bus,
                      kFastBackoff, kFastBackoff, kFastReconcile, kFastPump, kFastBrowse);
    client.Start();

    ASSERT_TRUE(WaitFor([&] { return client.Snapshot().state == KodiConnectionState::kConnected; }, 600));
    // app_version comes from the reconcile poll's Application.GetProperties
    // reply and nothing else touches it - a stable proof that frames off
    // an actual socket were correlated by id.
    EXPECT_EQ(client.Snapshot().app_version, "21.2");
    // muted comes only from the pushed Application.OnVolumeChanged - proof
    // the 0ms drain saw a genuinely buffered frame through the
    // libcurl-backed backend ([[feedback-fake-doubles-hide-backend-timing-bugs]]).
    ASSERT_TRUE(WaitFor([&] { return client.Snapshot().muted; }, 600));
    client.Stop();
}

}  // namespace
