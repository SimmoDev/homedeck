#include "core/harmony_connection.h"

#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace {

// Same scriptable-response HttpClient double as
// tests/weather_provider_test.cpp's FakeHttpClient, extended with Post()
// (HarmonyConnection's handshake is a POST, never a GET).
class FakeHttpClient : public homedeck::HttpClient {
public:
    homedeck::HttpClientResponse Get(const std::string& /*url*/) override { return homedeck::HttpClientResponse{false, 0, ""}; }

    homedeck::HttpClientResponse Post(const std::string& /*url*/, const std::string& /*json_body*/,
                                       const std::vector<std::pair<std::string, std::string>>& /*extra_headers*/ = {}) override {
        std::lock_guard<std::mutex> lock(mutex_);
        post_count_++;
        return response_;
    }

    void SetResponse(homedeck::HttpClientResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
    }

    int PostCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return post_count_;
    }

private:
    std::mutex mutex_;
    homedeck::HttpClientResponse response_{false, 0, ""};
    int post_count_ = 0;
};

// Shared control block every FakeWebSocketClient instance this test's
// factory hands out reads/writes through - HarmonyConnection creates a
// fresh WebSocketClient per (re)connect attempt, so a single fake
// instance can't be handed to the constructor the way FakeHttpClient is.
struct WsScript {
    std::mutex mutex;
    bool connect_should_succeed = true;
    bool send_should_succeed = true;
    std::vector<std::string> connect_urls;
    std::vector<std::string> sent_texts;
    std::deque<std::string> responses;
    int close_count = 0;
};

class FakeWebSocketClient : public homedeck::WebSocketClient {
public:
    explicit FakeWebSocketClient(std::shared_ptr<WsScript> script) : script_(std::move(script)) {}

    bool Connect(const std::string& url) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->connect_urls.push_back(url);
        return script_->connect_should_succeed;
    }

    bool SendText(const std::string& text) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->sent_texts.push_back(text);
        return script_->send_should_succeed;
    }

    std::optional<std::string> ReceiveText(int /*timeout_ms*/) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        if (script_->responses.empty()) {
            return std::nullopt;
        }
        std::string response = std::move(script_->responses.front());
        script_->responses.pop_front();
        return response;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->close_count++;
    }

private:
    std::shared_ptr<WsScript> script_;
};

// Polls Snapshot()/a predicate from the test thread until it holds or a
// bounded number of short sleeps elapse - same pattern
// weather_provider_test.cpp's WaitFor() uses, since HarmonyConnection's
// state changes happen on its own background Task's thread.
template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 300) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

constexpr char kHandshakeSuccessBody[] =
    R"({"id":1,"msg":"OK","data":{"activeRemoteId":17389408,"email":"someone@example.com"}})";

constexpr char kConfigSuccessBody[] =
    R"({"data":{"device":[{"id":"1","label":"TV"}],"activity":[{"id":"-1","label":"Off"},{"id":"123","label":"Watch TV"}]}})";

class HarmonyConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_harmony_connection_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
};

constexpr std::chrono::milliseconds kFastBackoff = std::chrono::milliseconds(30);

}  // namespace

TEST_F(HarmonyConnectionTest, NotConfiguredStaysDisconnectedAndNeverCallsOut) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::EventBus bus;
    FakeHttpClient http_client;
    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kDisconnected);
    EXPECT_EQ(http_client.PostCount(), 0);
    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ConfiguredHandshakeAndConfigFetchSucceedPublishesConnectedWithDevicesAndActivities) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
    }

    std::atomic<int> connected_events{0};
    std::atomic<int> config_events{0};
    auto state_sub = bus.Subscribe<homedeck::HarmonyConnectionStateChangedEvent>(
        [&connected_events](const homedeck::HarmonyConnectionStateChangedEvent& event) {
            if (event.state == homedeck::HarmonyConnectionState::kConnected) connected_events++;
        });
    auto config_sub = bus.Subscribe<homedeck::HarmonyConfigUpdatedEvent>(
        [&config_events](const homedeck::HarmonyConfigUpdatedEvent&) { config_events++; });

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_TRUE(WaitFor([&] { return connected_events.load() > 0; }));
    ASSERT_TRUE(WaitFor([&] { return config_events.load() > 0; }));

    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    EXPECT_EQ(snapshot.state, homedeck::HarmonyConnectionState::kConnected);
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].id, "1");
    EXPECT_EQ(snapshot.devices[0].label, "TV");
    ASSERT_EQ(snapshot.activities.size(), 2u);
    EXPECT_EQ(snapshot.activities[1].id, "123");
    EXPECT_EQ(snapshot.activities[1].label, "Watch TV");

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        ASSERT_FALSE(script->connect_urls.empty());
        EXPECT_NE(script->connect_urls.front().find("hubId=17389408"), std::string::npos);
    }

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, HandshakeFailureEntersErrorStateThenRecoversOnceItSucceeds) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});  // handshake fails

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
    }

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    // Fix the handshake - the retry loop (kFastBackoff later) should pick
    // it up on its own without any external trigger.
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    EXPECT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, TriggerReconnectPicksUpANewlySavedAddressImmediately) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // Deliberately not configured yet - the unconfigured path's own
    // 5s recheck interval is real and not injectable (see
    // harmony_connection.h), so this test relies on TriggerReconnect()
    // to short-circuit that wait rather than waiting one out.

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
    }

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kDisconnected; }));
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }, /*max_attempts=*/500));

    connection.Stop();
}
