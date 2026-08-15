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

#include <filesystem>
#include <memory>

namespace {

using homedeck::testing::HttpRequestRaw;

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
    ASSERT_TRUE(server.Start(18330));

    EXPECT_EQ(HttpRequestRaw(18330, "GET", "/api/harmony/status", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18330, "POST", "/api/harmony/reconnect", "").status_code, 403);

    auto setup = HttpRequestRaw(18330, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    EXPECT_EQ(HttpRequestRaw(18330, "GET", "/api/harmony/status", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18330, "POST", "/api/harmony/reconnect", "").status_code, 401);
}

TEST_F(HarmonyRoutesTest, StatusReportsDisconnectedWithNoConfigBeforeAnyHubIsConfigured) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, *harmony_connection_, *auth_);
    ASSERT_TRUE(server.Start(18331));
    std::string cookie = Login(18331);

    auto result = HttpRequestRaw(18331, "GET", "/api/harmony/status", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("state":"disconnected")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("hasConfig":false)"), std::string::npos);
}

TEST_F(HarmonyRoutesTest, ReconnectReturnsOk) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterHarmonyRoutes(server, *harmony_connection_, *auth_);
    ASSERT_TRUE(server.Start(18332));
    std::string cookie = Login(18332);

    auto result = HttpRequestRaw(18332, "POST", "/api/harmony/reconnect", "", cookie);
    EXPECT_EQ(result.status_code, 200);
}
