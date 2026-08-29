#include "core/harmony_routes.h"

#include "core/admin_auth_service.h"
#include "core/harmony_connection.h"
#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"

#include "http_test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace {

using homedeck::testing::HttpRequestRaw;

// Same "poll from the test thread" pattern as
// harmony_connection_test.cpp's own WaitFor() - HarmonyConnection's state
// changes happen on its own background Task's thread.
template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 300) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

// HarmonyConnection needs a WebSocketClient factory even though these
// route tests never actually connect (no hub address is ever configured
// here) - a factory that's never called is enough.
class UnusedWebSocketClient : public homedeck::WebSocketClient {
public:
    bool Connect(const std::string&) override { return false; }
    bool SendText(const std::string&) override { return false; }
    std::optional<std::string> ReceiveText(int) override { return std::nullopt; }
    void Close() override {}
};

class NeverCalledHttpClient : public homedeck::HttpClient {
public:
    homedeck::HttpClientResponse Get(const std::string&) override { return {false, 0, ""}; }
    homedeck::HttpClientResponse Post(const std::string&, const std::string&,
                                       const std::vector<std::pair<std::string, std::string>>& = {}) override {
        return {false, 0, ""};
    }
};

// A fixed-success handshake response - enough to get HarmonyConnection
// past ConnectAndFetchConfig()'s own handshake guard. Unlike
// harmony_connection_test.cpp's own scriptable FakeHttpClient, this file
// only needs one connect to stick (to prove SnapshotToJson() serializes a
// populated snapshot correctly), not to script a sequence of distinct
// responses, so it doesn't need that file's heavier machinery.
class FixedSuccessHttpClient : public homedeck::HttpClient {
public:
    homedeck::HttpClientResponse Get(const std::string&) override { return {false, 0, ""}; }
    homedeck::HttpClientResponse Post(const std::string&, const std::string&,
                                       const std::vector<std::pair<std::string, std::string>>& = {}) override {
        return {true, 200, R"({"id":1,"msg":"OK","data":{"activeRemoteId":17389408}})"};
    }
};

// A fixed-success WebSocket double that keeps serving the same populated
// config/current-activity payload for every real (non-zero-timeout)
// receive - both ConnectAndFetchConfig()'s config fetch and
// FetchCurrentActivity()'s own request read from the same "data" object,
// so one combined body satisfies both regardless of call order, and
// staying stable across repeat calls means the connection keeps passing
// its own periodic liveness probe rather than erroring out from under
// this test. A timeout of 0 (DrainStaleMessages()'s own poll) always
// returns nothing, the same "nothing buffered" behavior a real socket
// would have here.
class StableConnectedWebSocketClient : public homedeck::WebSocketClient {
public:
    bool Connect(const std::string&) override { return true; }
    bool SendText(const std::string&) override { return true; }
    std::optional<std::string> ReceiveText(int timeout_ms) override {
        if (timeout_ms == 0) return std::nullopt;
        return std::string(
            R"({"data":{)"
            R"("device":[{"id":"1","label":"TV","controlGroup":[]}],)"
            R"("activity":[{"id":"-1","label":"Off"},{"id":"123","label":"Watch TV"}],)"
            R"("result":"-1")"
            R"(}})");
    }
    void Close() override {}
};

class HarmonyRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_harmony_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
        event_bus_ = std::make_unique<homedeck::EventBus>();
        harmony_connection_ = std::make_unique<homedeck::HarmonyConnection>(
            http_client_, [] { return std::make_unique<UnusedWebSocketClient>(); }, *storage_, *event_bus_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::string Login(uint16_t port) {
        auto setup = HttpRequestRaw(port, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
        return homedeck::testing::SessionCookieOnly(setup.set_cookie);
    }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    homedeck::HostTimeSource time_source_;
    std::unique_ptr<homedeck::AdminAuthService> auth_;
    std::unique_ptr<homedeck::EventBus> event_bus_;
    NeverCalledHttpClient http_client_;
    std::unique_ptr<homedeck::HarmonyConnection> harmony_connection_;
};

}  // namespace

TEST_F(HarmonyRoutesTest, BothEndpointsRequireAuthentication) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, *harmony_connection_, *auth_);
    ASSERT_TRUE(server.Start(0));

    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "GET", "/api/harmony/status", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/harmony/reconnect", "").status_code, 403);

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "GET", "/api/harmony/status", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/harmony/reconnect", "").status_code, 401);
}

TEST_F(HarmonyRoutesTest, StatusReportsDisconnectedWithNoConfigBeforeAnyHubIsConfigured) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, *harmony_connection_, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto result = HttpRequestRaw(server.BoundPort(), "GET", "/api/harmony/status", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("state":"disconnected")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("hasConfig":false)"), std::string::npos);
}

// StatusReportsDisconnectedWithNoConfigBeforeAnyHubIsConfigured above only
// exercises SnapshotToJson()'s empty-state branch - the device/activity
// array population and currentActivityId it wires up once a hub is
// actually configured had no dedicated coverage through the real
// GET /api/harmony/status wire format.
TEST_F(HarmonyRoutesTest, StatusReportsPopulatedSnapshotOnceConnected) {
    ASSERT_TRUE(storage_->SetSetting("harmony", "hub_host", 1, "127.0.0.1"));
    FixedSuccessHttpClient connected_http_client;
    homedeck::HarmonyConnection connected_connection(
        connected_http_client, [] { return std::make_unique<StableConnectedWebSocketClient>(); }, *storage_,
        *event_bus_);
    connected_connection.Start();
    ASSERT_TRUE(WaitFor([&] { return connected_connection.Snapshot().has_config; }));

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, connected_connection, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto result = HttpRequestRaw(server.BoundPort(), "GET", "/api/harmony/status", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("state":"connected")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("hasConfig":true)"), std::string::npos);
    EXPECT_NE(result.body.find(R"("id":"1","label":"TV")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("id":"123","label":"Watch TV")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("currentActivityId":"-1")"), std::string::npos);

    connected_connection.Stop();
}

TEST_F(HarmonyRoutesTest, ReconnectReturnsOk) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, *harmony_connection_, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto result = HttpRequestRaw(server.BoundPort(), "POST", "/api/harmony/reconnect", "", cookie);
    EXPECT_EQ(result.status_code, 200);
}
