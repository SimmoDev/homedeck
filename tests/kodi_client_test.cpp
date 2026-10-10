#include "core/kodi_client.h"

#include "core/notification.h"
#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/websocket_client.h"
#include "platform/mdns_browser.h"
#include "third_party/nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <functional>

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
    // method -> builds the whole reply body ("result" or "error" member) from
    // the request, for tests that depend on the request's params (paging).
    std::map<std::string, std::function<nlohmann::json(const nlohmann::json&)>> handlers;
    // When set and true for a request, the fake sends no reply at all (a
    // slow server), so the caller times out on a connection that stays open.
    std::function<bool(const nlohmann::json&)> drop_request;
    // When true, a blocking ReceiveText() with nothing to deliver waits out
    // its timeout (or a delayed reply coming due) like a real transport,
    // instead of returning at once.
    bool real_wait = false;
    // method -> how long the fake holds that method's reply back.
    std::map<std::string, std::chrono::milliseconds> reply_delay;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> delayed;
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
            if (script_->drop_request && script_->drop_request(request)) {
                return true;
            }
            const std::string method = request["method"].get<std::string>();
            auto handler = script_->handlers.find(method);
            if (handler != script_->handlers.end()) {
                nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
                reply.update(handler->second(request), /*merge_objects=*/false);
                script_->ready.push_back(reply.dump());
                return true;
            }
            auto delay = script_->reply_delay.find(method);
            if (delay != script_->reply_delay.end()) {
                nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
                auto result = script_->results.find(method);
                reply["result"] = result != script_->results.end()
                                      ? nlohmann::json::parse(result->second, nullptr, false)
                                      : nlohmann::json::object();
                script_->delayed.emplace_back(std::chrono::steady_clock::now() + delay->second, reply.dump());
                return true;
            }
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
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(script_->mutex);
                if (script_->dead) {
                    return std::nullopt;
                }
                while (!script_->delayed.empty() && script_->delayed.front().first <= std::chrono::steady_clock::now()) {
                    script_->ready.push_back(std::move(script_->delayed.front().second));
                    script_->delayed.pop_front();
                }
                const bool deliverable =
                    !script_->pushed.empty() || (timeout_ms != 0 && !script_->ready.empty());
                if (deliverable || !script_->real_wait || timeout_ms == 0 ||
                    std::chrono::steady_clock::now() >= give_up) {
                    return TakeLocked(timeout_ms);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::optional<std::string> TakeLocked(int timeout_ms) {
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

    bool IsOpen() const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return !script_->dead;
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
                                           std::chrono::milliseconds max_command_age = std::chrono::seconds(5),
                                           std::chrono::milliseconds library_busy_after = std::chrono::seconds(3),
                                           std::chrono::milliseconds library_call_timeout = std::chrono::seconds(30)) {
        return std::make_unique<KodiClient>(
            [script] { return std::make_unique<FakeWebSocketClient>(script); }, browser, storage, bus, kFastBackoff,
            kFastBackoff, reconcile, kFastPump, kFastBrowse, max_command_age, library_busy_after,
            library_call_timeout);
    }

    static constexpr std::chrono::seconds kNoReconcile{30};

    std::filesystem::path root_dir_;
};

TEST(IsValidKodiSettingTest, ValidatesOnlyTheHostKey) {
    EXPECT_TRUE(homedeck::IsValidKodiSetting(KodiClient::kHostKey, "10.0.0.5"));
    EXPECT_FALSE(homedeck::IsValidKodiSetting(KodiClient::kHostKey, "http://10.0.0.5"));
    EXPECT_TRUE(homedeck::IsValidKodiSetting(KodiClient::kInstanceUuidKey, "any value at all"));
}

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

// An IPv6 literal is bracketed in both the ws:// authority and the host
// shown on the Web UI settings page.
TEST_F(KodiClientTest, AnIpv6OnlyInstanceIsConnectedThroughABracketedAuthority) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Shield", "2001:db8::5", "uuid-a")});
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    EXPECT_EQ(client->Snapshot().resolved_host, "[2001:db8::5]:9090");
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->connect_urls.front(), "ws://[2001:db8::5]:9090/jsonrpc");
    }
    client->Stop();
}

// The same Kodi can answer once per interface and IP protocol; that is one
// instance, so it is still auto-selected, and over its IPv4 address.
TEST_F(KodiClientTest, ADualStackInstanceAnsweringTwiceIsOneInstance) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    browser.SetInstances({Instance("Shield", "fe80::1", "uuid-a"), Instance("Shield", "10.0.30.20", "uuid-a")});
    auto script = std::make_shared<WsScript>();
    ScriptIdleKodi(script);

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    EXPECT_EQ(client->Snapshot().discovered.size(), 1u);
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

// The loop sleeps between target checks when nothing resolves; Stop() must
// not wait that out.
TEST_F(KodiClientTest, StopDuringTheNoTargetWaitReturnsPromptly) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));

    const auto started = std::chrono::steady_clock::now();
    client->Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

// Repeating the discovery pass with nothing found is not a state change.
TEST_F(KodiClientTest, RepeatedDiscoveryWithNoTargetPublishesNoRepeatedStateEvents) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();

    std::atomic<int> events{0};
    auto sub = bus.Subscribe<homedeck::KodiConnectionStateChangedEvent>(
        [&](const homedeck::KodiConnectionStateChangedEvent&) { ++events; });

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 4; }));
    client->Stop();

    EXPECT_EQ(events.load(), 0);  // the module starts in kDisconnected and never leaves it
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
    EXPECT_TRUE(client->Snapshot().target_configured);
    EXPECT_EQ(client->Snapshot().selected_uuid, "uuid-gone");
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty())
            << "the selected instance being offline must not silently control a different room";
    }
    client->Stop();
}

// Same address-less guard as DiscoveredInstanceWithNoAddressIsNotAutoSelected,
// exercised through the saved-uuid match path instead of auto-select -
// a saved selection matching a hostname-only instance must not connect,
// the same "don't silently guess" reasoning as an offline saved uuid.
TEST_F(KodiClientTest, SavedUuidMatchingAnAddresslessInstanceIsNotConnected) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kInstanceUuidKey, 1, "shield-uuid"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    MdnsService hostname_only;
    hostname_only.instance_name = "Shield";
    hostname_only.hostname = "Android";
    hostname_only.port = 9090;
    hostname_only.txt["uuid"] = "shield-uuid";
    browser.SetInstances({hostname_only});
    auto script = std::make_shared<WsScript>();

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty());
    }
    client->Stop();
}

// A discovered instance with no resolved IP address - just a bare mDNS
// hostname, e.g. a real device that advertises "Android" with no domain
// suffix at all - must not be auto-selected or connected to: MdnsService's
// own header warns ".local" resolution isn't guaranteed on either target,
// and this specific case can never resolve at all. Still shown in
// `discovered` so the Web UI can display it (and the user can enter its
// real address manually), just never used as a connect target.
TEST_F(KodiClientTest, DiscoveredInstanceWithNoAddressIsNotAutoSelected) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    MdnsService hostname_only;
    hostname_only.instance_name = "Shield";
    hostname_only.hostname = "Android";  // no `address` - the real-world failure case
    hostname_only.port = 9090;
    hostname_only.txt["uuid"] = "shield-uuid";
    browser.SetInstances({hostname_only});
    auto script = std::make_shared<WsScript>();

    auto client = MakeClient(script, browser, storage, bus);
    client->Start();

    ASSERT_TRUE(WaitFor([&] { return browser.BrowseCount() >= 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kDisconnected);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty())
            << "a hostname with no address must never be auto-connected to";
    }
    ASSERT_EQ(client->Snapshot().discovered.size(), 1u) << "still shown for the user to see/pick manually";
    EXPECT_EQ(client->Snapshot().discovered[0].host, "Android");
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

// ApplyItemFields() is merge-only, so a new item starting without an
// intervening Player.OnStop (a playlist advance, or just starting a
// movie mid-episode) would otherwise keep the previous item's stale
// show/season/episode. Player.OnPlay must clear identity before applying
// the new notification's own item.
TEST_F(KodiClientTest, OnPlayForANewItemClearsThePreviousItemsStaleIdentity) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting(KodiClient::kModuleId, KodiClient::kHostKey, 1, "10.0.30.20"));

    homedeck::EventBus bus;
    FakeMdnsBrowser browser;
    auto script = std::make_shared<WsScript>();
    ScriptPlayingKodi(script);  // an active player exists, so the immediate
                                // post-notification reconcile poll doesn't wipe identity

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPlay","params":{"data":{"item":{"title":"An Ep",)"
                 R"("showtitle":"The Show","season":10,"episode":7,"type":"episode"},"player":{"playerid":1,)"
                 R"("speed":1}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.season == 10; }));

    // A movie now starts - no OnStop, and its item has no show/season/episode.
    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPlay","params":{"data":{"item":{"title":"A Movie",)"
                 R"("type":"movie"},"player":{"playerid":1,"speed":1}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.title == "A Movie"; }));
    auto np = client->Snapshot().now_playing;
    EXPECT_TRUE(np.show_title.empty()) << "the episode's show title must not survive into the movie";
    EXPECT_EQ(np.season, -1);
    EXPECT_EQ(np.episode, -1);
    EXPECT_EQ(np.media_type, "movie");
    client->Stop();
}

// Per kodi.md's "Identity vs. timing" (ADR-0030 records the underlying
// protocol facts): once a Player.On* notification supplies identity, a
// later reconcile poll's Player.GetItem must not overwrite it - the
// main reason is add-on playback, where GetItem
// returns blanks that ApplyItemFields() would already leave alone, so
// this scripts the poll returning a *different, non-blank* title
// instead - the only way to prove identity_from_notification_
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

// A notification missed during an outage must not leave the previous
// connection's identity on screen: the reconnect poll repopulates it.
TEST_F(KodiClientTest, IdentityAfterAReconnectReflectsWhatIsPlayingNow) {
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

    Push(script, R"({"jsonrpc":"2.0","method":"Player.OnPlay","params":{"data":{"item":{"title":"An Ep",)"
                 R"("showtitle":"The Show","season":10,"episode":7,"type":"episode"},"player":{"playerid":1,)"
                 R"("speed":1}}}})");
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.season == 10; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Player.GetItem"] = R"({"item":{"title":"Movie B","type":"movie"}})";
    }
    client->TriggerReconnect();  // stands in for a link that dropped while the item changed
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->connect_urls.size() >= 2;
    }));
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto np = client->Snapshot().now_playing;
    EXPECT_EQ(np.title, "Movie B");
    EXPECT_TRUE(np.show_title.empty());
    EXPECT_EQ(np.season, -1);
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

// The same guard for a paused player: position/duration/percent are
// frozen while paused, so a reconcile poll that re-reads the identical
// snapshot must not fire KodiNowPlayingChangedEvent every cycle. (During
// uninterrupted playback position_ms advances, so the event still fires
// then - that path is covered by the tests above.)
TEST_F(KodiClientTest, PausedReconcilePollsDoNotRepublishWhenNothingChanged) {
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
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        // Paused: speed 0 and a fixed position that never advances.
        script->results["Player.GetProperties"] =
            R"({"speed":0,"percentage":25.0,"time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
            R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0},"canseek":true})";
    }

    auto client = MakeClient(script, browser, storage, bus, kFastReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.duration_ms == 20 * 60 * 1000; }));
    ASSERT_EQ(client->Snapshot().now_playing.playback, KodiPlaybackState::kPaused);

    // Let the initial poll's event settle, then measure across many cycles.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const int settled = now_playing_events.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_EQ(now_playing_events.load(), settled)
        << "an unchanging paused snapshot must not fire an event every reconcile cycle";
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
// buttons. Prove the poll threads it onto the snapshot for both
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

// Kodi's 9090 API has no authentication (ADR-0030), so a type-mismatched
// field in a Player.GetProperties reply isn't just a hypothetical - any
// device on the LAN can send one. nlohmann::json::value()/get<T>() throw
// on a type mismatch, which is std::abort() on firmware (exceptions are
// compiled out there): without GetInt()/GetDouble()/GetBool() guarding
// speed/percentage/canseek the same way they already guard
// volume/muted/version a few lines above, this test crashes the whole
// process rather than failing an assertion.
TEST_F(KodiClientTest, ReconcilePollWithTypeMismatchedPlayerPropertiesFallsBackInsteadOfCrashing) {
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

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.can_seek; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Player.GetProperties"] =
            R"({"speed":"fast","percentage":"half","time":{"hours":0,"minutes":5,"seconds":0,"milliseconds":0},)"
            R"("totaltime":{"hours":0,"minutes":20,"seconds":0,"milliseconds":0},"canseek":"false"})";
    }
    client->TriggerReconnect();
    // Reconnecting re-runs ConnectAndPrime()'s own initial ReconcilePoll
    // against the now-malformed script - the crash this guards against
    // happens inside that poll. The second connect is awaited first: the
    // state is already kConnected before the reconnect begins, so it alone
    // cannot show that the poll has run.
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->connect_urls.size() >= 2;
    }));
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }))
        << "a type-mismatched field must not abort the process before the connection can even settle";
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.position_ms == 5 * 60 * 1000; }))
        << "the well-typed fields of the same reply are still applied";

    // A reconnect starts from an empty snapshot, so every wrong-typed field
    // keeps its default rather than the previous connection's value.
    homedeck::KodiNowPlaying np = client->Snapshot().now_playing;
    EXPECT_EQ(np.speed, 0);
    EXPECT_DOUBLE_EQ(np.percent, 0.0);
    EXPECT_FALSE(np.can_seek);
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

// The Touch UI's rewind/fast-forward and volume +/- buttons use Kodi's
// own relative step verbs (not an absolute value computed from a
// possibly-stale snapshot), so rapid repeated taps stack server-side.
TEST_F(KodiClientTest, RelativeSeekAndVolumeStepsUseKodiStepVerbs) {
    KODI_COMMAND_RIG();
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->SeekStep(/*forward=*/true);
    ASSERT_TRUE(WaitFor(
        [&] { return SentFrameHasAll(script, {"Player.Seek", "\"playerid\":1", "\"value\":\"smallforward\""}); }));

    client->SeekStep(/*forward=*/false);
    ASSERT_TRUE(WaitFor(
        [&] { return SentFrameHasAll(script, {"Player.Seek", "\"playerid\":1", "\"value\":\"smallbackward\""}); }));

    client->VolumeStep(/*up=*/true);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Application.SetVolume", "\"volume\":\"increment\""}); }));

    client->VolumeStep(/*up=*/false);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Application.SetVolume", "\"volume\":\"decrement\""}); }));
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

// --- Library browse queries (M4b) ----------------------------------------

TEST_F(KodiClientTest, RequestMoviesFetchesAndParsesTheLibrary) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["VideoLibrary.GetMovies"] =
            R"({"movies":[{"movieid":1,"title":"Alpha","year":2020,"resume":{"position":0,"total":0}},)"
            R"({"movieid":2,"title":"Beta","year":2022,"resume":{"position":120.5,"total":6000.0}}]})";
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    ASSERT_EQ(received->size(), 2u);
    EXPECT_EQ((*received)[0].title, "Alpha");
    EXPECT_EQ((*received)[0].year, 2020);
    EXPECT_EQ((*received)[0].resume_position_ms, 0);
    EXPECT_EQ((*received)[1].title, "Beta");
    EXPECT_EQ((*received)[1].resume_position_ms, 120500)
        << "resume.position is seconds (float) - not the {hours,minutes,...} shape MillisFromTimeObject() parses";
    client->Stop();
}

// Every list parser shares the Get*/ResultArray() guards, but each one still
// has to be reached with entries of the wrong shape (not an object at all,
// or an object whose fields have the wrong types) and deliver an event
// rather than abort the process (std::abort() on firmware - see GetInt()'s
// comment in kodi_json.h).
TEST_F(KodiClientTest, EveryLibraryListToleratesMalformedEntriesInsteadOfCrashing) {
    KODI_COMMAND_RIG();
    const auto garbage = [](const std::string& key, const std::string& id_field) {
        return "{\"" + key + "\":[1,\"x\",null,[],{\"" + id_field +
               "\":\"bad\",\"title\":5,\"label\":{},\"year\":\"y\",\"episode\":[],\"season\":\"s\","
               "\"resume\":\"r\",\"file\":5,\"filetype\":[],\"duration\":{},\"track\":null}]}";
    };
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["VideoLibrary.GetMovies"] = garbage("movies", "movieid");
        script->results["VideoLibrary.GetTVShows"] = garbage("tvshows", "tvshowid");
        script->results["VideoLibrary.GetSeasons"] = garbage("seasons", "season");
        script->results["VideoLibrary.GetEpisodes"] = garbage("episodes", "episodeid");
        script->results["AudioLibrary.GetArtists"] = garbage("artists", "artistid");
        script->results["AudioLibrary.GetAlbums"] = garbage("albums", "albumid");
        script->results["AudioLibrary.GetSongs"] = garbage("songs", "songid");
        script->results["Files.GetSources"] = garbage("sources", "file");
        script->results["Files.GetDirectory"] = garbage("files", "file");
        script->results["PVR.GetChannelGroups"] = garbage("channelgroups", "channelgroupid");
        script->results["PVR.GetChannels"] = garbage("channels", "channelid");
    }

    std::atomic<int> events{0};
    auto s1 = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent&) { events++; });
    auto s2 = bus.Subscribe<homedeck::KodiTvShowsFetchedEvent>([&](const homedeck::KodiTvShowsFetchedEvent&) { events++; });
    auto s3 = bus.Subscribe<homedeck::KodiSeasonsFetchedEvent>([&](const homedeck::KodiSeasonsFetchedEvent&) { events++; });
    auto s4 = bus.Subscribe<homedeck::KodiEpisodesFetchedEvent>([&](const homedeck::KodiEpisodesFetchedEvent&) { events++; });
    auto s5 = bus.Subscribe<homedeck::KodiArtistsFetchedEvent>([&](const homedeck::KodiArtistsFetchedEvent&) { events++; });
    auto s6 = bus.Subscribe<homedeck::KodiAlbumsFetchedEvent>([&](const homedeck::KodiAlbumsFetchedEvent&) { events++; });
    auto s7 = bus.Subscribe<homedeck::KodiSongsFetchedEvent>([&](const homedeck::KodiSongsFetchedEvent&) { events++; });
    auto s8 = bus.Subscribe<homedeck::KodiFilesFetchedEvent>([&](const homedeck::KodiFilesFetchedEvent&) { events++; });
    auto s9 = bus.Subscribe<homedeck::KodiChannelGroupsFetchedEvent>(
        [&](const homedeck::KodiChannelGroupsFetchedEvent&) { events++; });
    auto s10 = bus.Subscribe<homedeck::KodiChannelsFetchedEvent>([&](const homedeck::KodiChannelsFetchedEvent&) { events++; });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    // One at a time: the pending queue holds at most
    // kMaxPendingLibraryRequests, so firing all eleven at once would drop
    // the oldest. Files.GetSources and GetDirectory both publish
    // KodiFilesFetchedEvent.
    int expected = 0;
    const auto request = [&](const std::function<void()>& send) {
        send();
        ++expected;
        return WaitFor([&] { return events.load() == expected; });
    };
    EXPECT_TRUE(request([&] { client->RequestMovies(); }));
    EXPECT_TRUE(request([&] { client->RequestTvShows(); }));
    EXPECT_TRUE(request([&] { client->RequestSeasons(1); }));
    EXPECT_TRUE(request([&] { client->RequestEpisodes(1, 1); }));
    EXPECT_TRUE(request([&] { client->RequestArtists(); }));
    EXPECT_TRUE(request([&] { client->RequestAlbums(1); }));
    EXPECT_TRUE(request([&] { client->RequestSongs(1); }));
    EXPECT_TRUE(request([&] { client->RequestFileSources(); }));
    EXPECT_TRUE(request([&] { client->RequestDirectory("/x"); }));
    EXPECT_TRUE(request([&] { client->RequestChannelGroups(); }));
    EXPECT_TRUE(request([&] { client->RequestChannels(1); }));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    client->Stop();
}

// Notification payloads are as untrusted as replies: params/data/item/player
// of the wrong JSON type must be ignored, not abort or wedge the loop.
TEST_F(KodiClientTest, NotificationsWithWronglyTypedPayloadsAreIgnoredInsteadOfCrashing) {
    KODI_COMMAND_RIG();
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    Push(script, R"({"method":"Player.OnPlay","params":"str"})");
    Push(script, R"({"method":"Player.OnPlay","params":{"data":[1]}})");
    Push(script, R"({"method":"Player.OnPlay","params":{"data":{"item":"str","player":{"speed":"fast"}}}})");
    Push(script, R"({"method":"Player.OnPlay","params":{"data":{"item":{"title":5,"season":"x","type":[]},"player":5}}})");
    Push(script, R"({"method":"Application.OnVolumeChanged","params":{"data":{"volume":"loud","muted":1}}})");
    Push(script, R"({"method":5,"params":{}})");
    Push(script, R"([1,2,3])");
    // A well-formed notification afterwards proves the loop kept draining.
    // Identity (not volume): the immediate reconcile poll an OnPlay triggers
    // re-reads volume from the static fake, but never overwrites a
    // notification-supplied title.
    Push(script, R"({"method":"Player.OnPlay","params":{"data":{"item":{"title":"Still Alive","type":"movie"}}}})");

    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().now_playing.title == "Still Alive"; }));
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    client->Stop();
}

// A library listing is fetched in kLibraryPageSize pages so no single reply
// can approach kMaxWebSocketMessageBytes: a 1,200-movie library takes three
// requests and arrives as one merged event.
TEST_F(KodiClientTest, RequestMoviesFetchesALargeLibraryInPagesAndMergesThem) {
    KODI_COMMAND_RIG();
    constexpr int kTotal = 1200;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [kTotal](const nlohmann::json& request) {
            const long long start = request["params"]["limits"]["start"].get<long long>();
            const long long end = std::min<long long>(request["params"]["limits"]["end"].get<long long>(), kTotal);
            nlohmann::json movies = nlohmann::json::array();
            for (long long i = start; i < end; ++i) {
                movies.push_back({{"movieid", i}, {"title", "Movie " + std::to_string(i)}, {"year", 2000}});
            }
            return nlohmann::json{
                {"result", {{"movies", movies}, {"limits", {{"start", start}, {"end", end}, {"total", kTotal}}}}}};
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    bool truncated = true;  // overwritten by the event
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
        truncated = event.truncated;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_EQ(received->size(), static_cast<size_t>(kTotal));
        EXPECT_EQ(received->front().title, "Movie 0");
        EXPECT_EQ(received->back().title, "Movie 1199");
    EXPECT_FALSE(truncated);
    }
    EXPECT_EQ(CountSent(script, "VideoLibrary.GetMovies"), 3);
    client->Stop();
}

// A Kodi that rejects the `limits` parameter must still deliver its library.
// A listing occupies the loop thread request by request, so a playback
// command queued while it is paging goes out between pages, not after the
// last one.
TEST_F(KodiClientTest, ACommandQueuedDuringAPagedListingIsSentBetweenPages) {
    KODI_COMMAND_RIG();
    constexpr int kTotal = 1200;
    std::atomic<KodiClient*> client_ptr{nullptr};
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [kTotal, &client_ptr](const nlohmann::json& request) {
            const long long start = request["params"]["limits"]["start"].get<long long>();
            const long long end = std::min<long long>(request["params"]["limits"]["end"].get<long long>(), kTotal);
            if (start == 500) {
                client_ptr.load()->PlayPause();
            }
            nlohmann::json movies = nlohmann::json::array();
            for (long long i = start; i < end; ++i) {
                movies.push_back({{"movieid", i}, {"title", "Movie"}});
            }
            return nlohmann::json{
                {"result", {{"movies", movies}, {"limits", {{"start", start}, {"end", end}, {"total", kTotal}}}}}};
        };
    }
    std::atomic<bool> done{false};
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>(
        [&](const homedeck::KodiMoviesFetchedEvent&) { done = true; });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client_ptr = client.get();
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] { return done.load(); }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        size_t play_pause = std::string::npos;
        size_t third_page = std::string::npos;
        for (size_t i = 0; i < script->sent.size(); ++i) {
            if (play_pause == std::string::npos && script->sent[i].find("Player.PlayPause") != std::string::npos) {
                play_pause = i;
            }
            if (script->sent[i].find("\"start\":1000") != std::string::npos) {
                third_page = i;
            }
        }
        ASSERT_NE(play_pause, std::string::npos);
        ASSERT_NE(third_page, std::string::npos);
        EXPECT_LT(play_pause, third_page);
    }
    client->Stop();
}

// Kodi answers nothing else while a listing is slow, so a reply that takes
// longer than the busy threshold raises library_busy until Kodi answers.
TEST_F(KodiClientTest, ASlowLibraryReplyRaisesLibraryBusyUntilKodiAnswers) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->real_wait = true;
        script->results["VideoLibrary.GetMovies"] = R"({"movies":[]})";
        script->reply_delay["VideoLibrary.GetMovies"] = std::chrono::milliseconds(500);
    }
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile, std::chrono::seconds(5),
                             std::chrono::milliseconds(100));
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    EXPECT_FALSE(client->Snapshot().library_busy);

    client->RequestMovies();
    EXPECT_TRUE(WaitFor([&] { return client->Snapshot().library_busy; }));
    EXPECT_TRUE(WaitFor([&] { return !client->Snapshot().library_busy; }));
    client->Stop();
}

// The reply to a call that already timed out is read by the idle pump, not by
// a Call() waiting for it; it still proves Kodi is no longer stuck.
TEST_F(KodiClientTest, ALateReplyReadWhileIdleClearsLibraryBusy) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->real_wait = true;
        script->drop_request = [](const nlohmann::json& request) {
            return request["method"].get<std::string>() == "VideoLibrary.GetMovies";
        };
    }
    std::atomic<bool> listing_ended{false};
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>(
        [&](const homedeck::KodiMoviesFetchedEvent&) { listing_ended = true; });
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile, std::chrono::seconds(5),
                             std::chrono::milliseconds(50), std::chrono::milliseconds(300));
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().library_busy; }));
    ASSERT_TRUE(WaitFor([&] { return listing_ended.load(); }));
    EXPECT_TRUE(client->Snapshot().library_busy);  // timed out, Kodi still silent

    Push(script, R"({"jsonrpc":"2.0","id":9999,"result":{}})");
    EXPECT_TRUE(WaitFor([&] { return !client->Snapshot().library_busy; }));
    client->Stop();
}

TEST_F(KodiClientTest, AFastLibraryReplyNeverRaisesLibraryBusy) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->real_wait = true;
        script->results["VideoLibrary.GetMovies"] = R"({"movies":[]})";
    }
    std::atomic<bool> ever_busy{false};
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile, std::chrono::seconds(5),
                             std::chrono::milliseconds(300));
    auto sub = bus.Subscribe<homedeck::KodiNowPlayingChangedEvent>(
        [&](const homedeck::KodiNowPlayingChangedEvent&) {
            if (client->Snapshot().library_busy) {
                ever_busy = true;
            }
        });
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    std::atomic<bool> done{false};
    auto movies_sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>(
        [&](const homedeck::KodiMoviesFetchedEvent&) { done = true; });
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] { return done.load(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    EXPECT_FALSE(ever_busy.load());
    EXPECT_FALSE(client->Snapshot().library_busy);
    client->Stop();
}

TEST_F(KodiClientTest, LibraryBusyClearsWhenTheConnectionDrops) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->real_wait = true;
        script->drop_request = [](const nlohmann::json& request) {
            return request["method"].get<std::string>() == "VideoLibrary.GetMovies";
        };
    }
    auto client = MakeClient(script, browser, storage, bus, kNoReconcile, std::chrono::seconds(5),
                             std::chrono::milliseconds(100));
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().library_busy; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->dead = true;
    }
    EXPECT_TRUE(WaitFor([&] { return !client->Snapshot().library_busy; }));
    client->Stop();
}

TEST_F(KodiClientTest, RequestMoviesRetriesUnpagedWhenKodiRejectsLimits) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [](const nlohmann::json& request) {
            if (request["params"].contains("limits")) {
                return nlohmann::json{{"error", {{"code", -32602}, {"message", "Invalid params."}}}};
            }
            return nlohmann::json{{"result", {{"movies", {{{"movieid", 7}, {"title", "Solo"}}}}}}};
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    ASSERT_EQ(received->size(), 1u);
    EXPECT_EQ((*received)[0].title, "Solo");
    client->Stop();
}

// A server reporting an endless library can't grow the merged list without
// bound.
TEST_F(KodiClientTest, RequestMoviesTruncatesAtTheLibraryItemCap) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [](const nlohmann::json& request) {
            const long long start = request["params"]["limits"]["start"].get<long long>();
            const long long end = request["params"]["limits"]["end"].get<long long>();
            nlohmann::json movies = nlohmann::json::array();
            for (long long i = start; i < end; ++i) {
                movies.push_back({{"movieid", i}, {"title", "M"}});
            }
            return nlohmann::json{{"result", {{"movies", movies}, {"limits", {{"total", 1000000}}}}}};
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    bool truncated = false;  // overwritten by the event
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
        truncated = event.truncated;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    EXPECT_EQ(received->size(), 10000u);
    EXPECT_TRUE(truncated);
    client->Stop();
}

// A page that fails after the first leaves an incomplete list, which the
// screen must be able to say so.
TEST_F(KodiClientTest, RequestMoviesReportsTruncationWhenALaterPageFails) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [](const nlohmann::json& request) {
            const long long start = request["params"]["limits"]["start"].get<long long>();
            if (start > 0) {
                return nlohmann::json{{"error", {{"code", -32000}, {"message", "Failed."}}}};
            }
            nlohmann::json movies = nlohmann::json::array();
            for (long long i = 0; i < 500; ++i) {
                movies.push_back({{"movieid", i}, {"title", "M"}});
            }
            return nlohmann::json{{"result", {{"movies", movies}, {"limits", {{"total", 1200}}}}}};
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    bool truncated = false;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
        truncated = event.truncated;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    EXPECT_EQ(received->size(), 500u);
    EXPECT_TRUE(truncated);
    client->Stop();
}

// A library call that gets no reply in time on a connection that is still
// open (a cold network share) must not drop the link: the screen gets an
// empty list flagged as incomplete and Now Playing stays connected.
TEST_F(KodiClientTest, ALibraryCallThatTimesOutOnAnOpenConnectionKeepsTheLink) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->drop_request = [](const nlohmann::json& request) {
            return request["method"].get<std::string>() == "VideoLibrary.GetMovies";
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    bool truncated = false;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
        truncated = event.truncated;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_TRUE(received->empty());
        EXPECT_TRUE(truncated);  // a timed-out listing is not an empty library
    }
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->connect_urls.size(), 1u);  // never reconnected
    }
    client->Stop();
}

// Kodi answers one call at a time, so a periodic poll that gets no reply on an
// open link means "busy", not "gone": the link must not be torn down and
// re-established while Kodi works through a slow call.
TEST_F(KodiClientTest, AReconcilePollThatTimesOutOnAnOpenConnectionKeepsTheLink) {
    KODI_COMMAND_RIG();
    // Drops the first two polls after `stalled` is set (fewer than
    // kMaxToleratedPollTimeouts), then answers again.
    auto stalled = std::make_shared<std::atomic<bool>>(false);
    auto dropped = std::make_shared<std::atomic<int>>(0);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->drop_request = [stalled, dropped](const nlohmann::json& request) {
            return stalled->load() && request["method"].get<std::string>() == "Application.GetProperties" &&
                   dropped->fetch_add(1) < 2;
        };
    }
    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    stalled->store(true);
    ASSERT_TRUE(WaitFor([&] { return dropped->load() >= 3; }));  // two dropped, then one answered
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->connect_urls.size(), 1u);  // never reconnected
    }
    client->Stop();
}

// A link that answers nothing for kMaxToleratedPollTimeouts polls in a row is
// dead even though the transport still reports itself open.
TEST_F(KodiClientTest, RepeatedReconcilePollTimeoutsOnAnOpenConnectionReconnect) {
    KODI_COMMAND_RIG();
    auto stalled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->drop_request = [stalled](const nlohmann::json&) { return stalled->load(); };
    }
    auto client = MakeClient(script, browser, storage, bus);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    stalled->store(true);
    EXPECT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->connect_urls.size() >= 2u;
    }));
    client->Stop();
}

// The unpaged lists (channel groups, file sources) flag a timeout too, so a
// screen can tell a slow listing from an empty one.
TEST_F(KodiClientTest, AChannelGroupListingThatTimesOutIsFlaggedTruncated) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->drop_request = [](const nlohmann::json& request) {
            return request["method"].get<std::string>() == "PVR.GetChannelGroups";
        };
    }

    std::mutex result_mutex;
    std::optional<homedeck::KodiChannelGroupsFetchedEvent> received;
    auto sub = bus.Subscribe<homedeck::KodiChannelGroupsFetchedEvent>(
        [&](const homedeck::KodiChannelGroupsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            received = event;
        });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestChannelGroups();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    EXPECT_TRUE(received->groups.empty());
    EXPECT_TRUE(received->truncated);
    client->Stop();
}

// Pages that arrived before a timeout are kept and flagged as incomplete.
TEST_F(KodiClientTest, ALibraryCallThatTimesOutAfterTheFirstPageKeepsThatPageAsTruncated) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->handlers["VideoLibrary.GetMovies"] = [](const nlohmann::json&) {
            nlohmann::json movies = nlohmann::json::array();
            for (long long i = 0; i < 500; ++i) {
                movies.push_back({{"movieid", i}, {"title", "M"}});
            }
            return nlohmann::json{{"result", {{"movies", movies}, {"limits", {{"total", 1200}}}}}};
        };
        script->drop_request = [](const nlohmann::json& request) {
            return request["method"].get<std::string>() == "VideoLibrary.GetMovies" &&
                   request["params"]["limits"]["start"].get<long long>() > 0;
        };
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    bool truncated = false;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
        truncated = event.truncated;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));
    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    EXPECT_EQ(received->size(), 500u);
    EXPECT_TRUE(truncated);
    EXPECT_EQ(client->Snapshot().state, KodiConnectionState::kConnected);
    client->Stop();
}

// Kodi's 9090 API has no authentication (ADR-0030), so a type-mismatched
// field isn't just a hypothetical - any device on the LAN can send one.
// nlohmann::json::value()/get<T>() throw on a type mismatch, which is
// std::abort() on firmware (exceptions are disabled there - see GetInt()'s
// own comment in kodi_json.cpp): a regression aborts the whole test
// binary rather than failing one assertion.
TEST_F(KodiClientTest, RequestMoviesWithATypeMismatchedFieldFallsBackToDefaultsInsteadOfCrashing) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["VideoLibrary.GetMovies"] =
            R"({"movies":[{"movieid":"not-a-number","title":42,"year":"two-thousand","resume":"oops"},)"
            R"({"movieid":2,"title":"Beta","year":2022,"resume":{"position":0,"total":0}}]})";
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiMovie>> received;
    auto sub = bus.Subscribe<homedeck::KodiMoviesFetchedEvent>([&](const homedeck::KodiMoviesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.movies;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestMovies();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    ASSERT_EQ(received->size(), 2u);
    EXPECT_EQ((*received)[0].movieid, -1) << "wrong-typed movieid falls back to the default, not a crash";
    EXPECT_EQ((*received)[0].title, "") << "title (42, not a string) and label (absent) both fall back to empty";
    EXPECT_EQ((*received)[0].year, 0) << "wrong-typed year falls back to 0";
    EXPECT_EQ((*received)[0].resume_position_ms, 0) << "non-object resume falls back to no resume point";
    EXPECT_EQ((*received)[1].title, "Beta") << "a well-formed sibling entry still parses correctly";
    client->Stop();
}

TEST_F(KodiClientTest, RequestTvShowsFetchesAndParsesTheLibrary) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["VideoLibrary.GetTVShows"] =
            R"({"tvshows":[{"tvshowid":5,"title":"Some Show","year":2018,"episode":24,"watchedepisodes":10}]})";
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiTvShow>> received;
    auto sub = bus.Subscribe<homedeck::KodiTvShowsFetchedEvent>([&](const homedeck::KodiTvShowsFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        received = event.shows;
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestTvShows();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return received.has_value();
    }));

    std::lock_guard<std::mutex> lock(result_mutex);
    ASSERT_EQ(received->size(), 1u);
    EXPECT_EQ((*received)[0].tvshowid, 5);
    EXPECT_EQ((*received)[0].title, "Some Show");
    EXPECT_EQ((*received)[0].episode_count, 24);
    EXPECT_EQ((*received)[0].watched_episode_count, 10);
    client->Stop();
}

TEST_F(KodiClientTest, RequestSeasonsAndEpisodesSendTheRightParamsAndParseTheReply) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["VideoLibrary.GetSeasons"] =
            R"({"seasons":[{"season":1,"label":"Season 1","episode":8,"watchedepisodes":8}]})";
        script->results["VideoLibrary.GetEpisodes"] =
            R"({"episodes":[{"episodeid":42,"episode":3,"title":"Ep Three",)"
            R"("resume":{"position":90.0,"total":1200.0}}]})";
    }

    std::mutex result_mutex;
    std::optional<homedeck::KodiSeasonsFetchedEvent> seasons_event;
    std::optional<homedeck::KodiEpisodesFetchedEvent> episodes_event;
    auto seasons_sub = bus.Subscribe<homedeck::KodiSeasonsFetchedEvent>(
        [&](const homedeck::KodiSeasonsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            seasons_event = event;
        });
    auto episodes_sub = bus.Subscribe<homedeck::KodiEpisodesFetchedEvent>(
        [&](const homedeck::KodiEpisodesFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            episodes_event = event;
        });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestSeasons(7);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return seasons_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"VideoLibrary.GetSeasons", "\"tvshowid\":7"}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(seasons_event->tvshowid, 7);
        ASSERT_EQ(seasons_event->seasons.size(), 1u);
        EXPECT_EQ(seasons_event->seasons[0].season, 1);
        EXPECT_EQ(seasons_event->seasons[0].label, "Season 1");
        EXPECT_EQ(seasons_event->seasons[0].episode_count, 8);
    }

    client->RequestEpisodes(7, 1);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return episodes_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"VideoLibrary.GetEpisodes", "\"tvshowid\":7", "\"season\":1"}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(episodes_event->tvshowid, 7);
        EXPECT_EQ(episodes_event->season, 1);
        ASSERT_EQ(episodes_event->episodes.size(), 1u);
        EXPECT_EQ(episodes_event->episodes[0].episodeid, 42);
        EXPECT_EQ(episodes_event->episodes[0].title, "Ep Three");
        EXPECT_EQ(episodes_event->episodes[0].resume_position_ms, 90000);
    }
    client->Stop();
}

TEST_F(KodiClientTest, ALibraryRequestQueuedWhileDisconnectedIsSentOnceConnected) {
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
        script->results["VideoLibrary.GetMovies"] = R"({"movies":[]})";
        script->connect_ok = false;
    }

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kError; }));

    client->RequestMovies();  // queued against a dead connection
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(CountSent(script, "VideoLibrary.GetMovies"), 0);

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = true;
    }
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "VideoLibrary.GetMovies") == 1; }))
        << "the queued request must go out once a connection exists";
    client->Stop();
}

TEST_F(KodiClientTest, IdenticalLibraryRequestsQueuedTogetherAreSentOnce) {
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
        script->results["VideoLibrary.GetMovies"] = R"({"movies":[]})";
        script->results["VideoLibrary.GetSeasons"] = R"({"seasons":[]})";
        script->connect_ok = false;
    }

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kError; }));

    client->RequestMovies();
    client->RequestMovies();
    client->RequestSeasons(7);
    client->RequestSeasons(8);  // a different show is a different request
    client->RequestSeasons(7);
    client->RequestMovies();

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_ok = true;
    }
    ASSERT_TRUE(WaitFor([&] { return CountSent(script, "VideoLibrary.GetSeasons") == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(CountSent(script, "VideoLibrary.GetMovies"), 1);
    EXPECT_EQ(CountSent(script, "VideoLibrary.GetSeasons"), 2);
    client->Stop();
}

TEST_F(KodiClientTest, RequestArtistsAlbumsAndSongsSendTheRightParamsAndParseTheReply) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["AudioLibrary.GetArtists"] =
            R"({"artists":[{"artistid":2,"artist":"3 Daft Monkeys","label":"3 Daft Monkeys"}]})";
        script->results["AudioLibrary.GetAlbums"] =
            R"({"albums":[{"albumid":1,"title":"Brouhaha","label":"Brouhaha","year":2000}]})";
        script->results["AudioLibrary.GetSongs"] =
            R"({"songs":[{"songid":1,"track":1,"title":"Wonderful","label":"Wonderful","duration":272}]})";
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiArtist>> artists;
    std::optional<homedeck::KodiAlbumsFetchedEvent> albums_event;
    std::optional<homedeck::KodiSongsFetchedEvent> songs_event;
    auto artists_sub = bus.Subscribe<homedeck::KodiArtistsFetchedEvent>(
        [&](const homedeck::KodiArtistsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            artists = event.artists;
        });
    auto albums_sub = bus.Subscribe<homedeck::KodiAlbumsFetchedEvent>(
        [&](const homedeck::KodiAlbumsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            albums_event = event;
        });
    auto songs_sub = bus.Subscribe<homedeck::KodiSongsFetchedEvent>(
        [&](const homedeck::KodiSongsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            songs_event = event;
        });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestArtists();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return artists.has_value();
    }));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_EQ(artists->size(), 1u);
        EXPECT_EQ((*artists)[0].artistid, 2);
        EXPECT_EQ((*artists)[0].name, "3 Daft Monkeys");
    }

    client->RequestAlbums(2);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return albums_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"AudioLibrary.GetAlbums", "\"artistid\":2"}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(albums_event->artistid, 2);
        ASSERT_EQ(albums_event->albums.size(), 1u);
        EXPECT_EQ(albums_event->albums[0].albumid, 1);
        EXPECT_EQ(albums_event->albums[0].title, "Brouhaha");
        EXPECT_EQ(albums_event->albums[0].year, 2000);
    }

    client->RequestSongs(1);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return songs_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"AudioLibrary.GetSongs", "\"albumid\":1"}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(songs_event->albumid, 1);
        ASSERT_EQ(songs_event->songs.size(), 1u);
        EXPECT_EQ(songs_event->songs[0].songid, 1);
        EXPECT_EQ(songs_event->songs[0].track, 1);
        EXPECT_EQ(songs_event->songs[0].title, "Wonderful");
        EXPECT_EQ(songs_event->songs[0].duration_seconds, 272);
    }
    client->Stop();
}

TEST_F(KodiClientTest, RequestFileSourcesAndDirectorySendTheRightParamsAndParseTheReply) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["Files.GetSources"] = R"({"sources":[{"file":"/mnt/nas/Media/Movies/","label":"Movies"}]})";
        script->results["Files.GetDirectory"] =
            R"({"files":[)"
            R"({"file":"/mnt/nas/Media/Movies/Show/","filetype":"directory","label":"Show"},)"
            R"({"file":"/mnt/nas/Media/Movies/Movie.mkv","filetype":"file","label":"Movie"}]})";
    }

    std::mutex result_mutex;
    std::optional<homedeck::KodiFilesFetchedEvent> sources_event;
    std::optional<homedeck::KodiFilesFetchedEvent> directory_event;
    auto files_sub = bus.Subscribe<homedeck::KodiFilesFetchedEvent>([&](const homedeck::KodiFilesFetchedEvent& event) {
        std::lock_guard<std::mutex> lock(result_mutex);
        if (event.path.empty()) {
            sources_event = event;
        } else {
            directory_event = event;
        }
    });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestFileSources();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return sources_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"Files.GetSources", "\"media\":\"video\""}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_EQ(sources_event->items.size(), 1u);
        EXPECT_EQ(sources_event->items[0].path, "/mnt/nas/Media/Movies/");
        EXPECT_EQ(sources_event->items[0].label, "Movies");
        EXPECT_TRUE(sources_event->items[0].is_folder) << "a source is always a folder, filetype absent or not";
    }

    client->RequestDirectory("/mnt/nas/Media/Movies/");
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return directory_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(
        script, {"Files.GetDirectory", "\"directory\":\"/mnt/nas/Media/Movies/\"", "\"media\":\"video\""}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(directory_event->path, "/mnt/nas/Media/Movies/");
        ASSERT_EQ(directory_event->items.size(), 2u);
        EXPECT_EQ(directory_event->items[0].label, "Show");
        EXPECT_TRUE(directory_event->items[0].is_folder);
        EXPECT_EQ(directory_event->items[1].label, "Movie");
        EXPECT_FALSE(directory_event->items[1].is_folder);
    }

    client->PlayFile("/mnt/nas/Media/Movies/Movie.mkv");
    ASSERT_TRUE(WaitFor([&] {
        return SentFrameHasAll(script, {"Player.Open", "\"file\":\"/mnt/nas/Media/Movies/Movie.mkv\""});
    }));
    client->Stop();
}

TEST_F(KodiClientTest, RequestChannelGroupsAndChannelsSendTheRightParamsAndParseTheReply) {
    KODI_COMMAND_RIG();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->results["PVR.GetChannelGroups"] =
            R"({"channelgroups":[{"channelgroupid":2,"channeltype":"tv","label":"All channels"}]})";
        script->results["PVR.GetChannels"] =
            R"({"channels":[{"channelid":34,"channeltype":"tv","label":"BBC One NW HD"}]})";
    }

    std::mutex result_mutex;
    std::optional<std::vector<homedeck::KodiChannelGroup>> groups;
    std::optional<homedeck::KodiChannelsFetchedEvent> channels_event;
    auto groups_sub = bus.Subscribe<homedeck::KodiChannelGroupsFetchedEvent>(
        [&](const homedeck::KodiChannelGroupsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            groups = event.groups;
        });
    auto channels_sub = bus.Subscribe<homedeck::KodiChannelsFetchedEvent>(
        [&](const homedeck::KodiChannelsFetchedEvent& event) {
            std::lock_guard<std::mutex> lock(result_mutex);
            channels_event = event;
        });

    auto client = MakeClient(script, browser, storage, bus, kNoReconcile);
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == KodiConnectionState::kConnected; }));

    client->RequestChannelGroups();
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return groups.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"PVR.GetChannelGroups", "\"channeltype\":\"tv\""}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        ASSERT_EQ(groups->size(), 1u);
        EXPECT_EQ((*groups)[0].channelgroupid, 2);
        EXPECT_EQ((*groups)[0].label, "All channels");
    }

    client->RequestChannels(2);
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(result_mutex);
        return channels_event.has_value();
    }));
    EXPECT_TRUE(SentFrameHasAll(script, {"PVR.GetChannels", "\"channelgroupid\":2"}));
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        EXPECT_EQ(channels_event->channelgroupid, 2);
        ASSERT_EQ(channels_event->channels.size(), 1u);
        EXPECT_EQ(channels_event->channels[0].channelid, 34);
        EXPECT_EQ(channels_event->channels[0].label, "BBC One NW HD");
    }

    client->OpenLibraryItem("channelid", 34, /*resume=*/false);
    ASSERT_TRUE(WaitFor([&] { return SentFrameHasAll(script, {"Player.Open", "\"channelid\":34"}); }));
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
// running on the developer's machine - the exact setup anyone
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
// frames, a 0ms drain that sees a buffered push) end to end. A scripted
// FakeWebSocketClient cannot show whether the real backend's zero-timeout
// receive behaves as modelled, so this one runs over sockets.
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
    // the 0ms drain saw a buffered frame through the libcurl-backed
    // backend.
    ASSERT_TRUE(WaitFor([&] { return client.Snapshot().muted; }, 600));
    client.Stop();
}

}  // namespace
