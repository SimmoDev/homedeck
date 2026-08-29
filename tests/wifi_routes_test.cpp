#include "core/wifi_routes.h"

#include "core/admin_auth_service.h"
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
using homedeck::testing::HttpResult;
using homedeck::testing::SessionCookieOnly;

class WifiRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_wifi_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    // Registers auth routes and returns the session cookie from a real
    // setup call - must run after server.Start(), per HttpServer's own
    // contract; callers register their own routes (and
    // RegisterAdminAuthRoutes) before calling Start(). Same precedent as
    // ota_routes_test.cpp/settings_routes_test.cpp.
    std::string Login(uint16_t port) {
        auto setup = HttpRequestRaw(port, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
        return SessionCookieOnly(setup.set_cookie);
    }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    homedeck::HostTimeSource time_source_;
    std::unique_ptr<homedeck::AdminAuthService> auth_;
};

}  // namespace

TEST_F(WifiRoutesTest, ResetRequiresAuthentication) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWifiRoutes(server, *auth_, []() -> std::optional<std::string> { return "HomeDeck-TEST"; });
    ASSERT_TRUE(server.Start(0));

    // No password set yet - 403 setup_required, matching
    // ota_routes_test.cpp/diagnostics_routes_test.cpp's precedent.
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/wifi/reset", "").status_code, 403);

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    // Password set, but no session cookie - 401.
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/wifi/reset", "").status_code, 401);
}

TEST_F(WifiRoutesTest, ResetSucceedsAndReturnsTheApSsidFromTheInjectedResetFunction) {
    bool reset_called = false;
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWifiRoutes(server, *auth_, [&reset_called]() -> std::optional<std::string> {
        reset_called = true;
        return "HomeDeck-A1B2C3";
    });
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto reset = HttpRequestRaw(server.BoundPort(), "POST", "/api/wifi/reset", "", cookie);
    EXPECT_EQ(reset.status_code, 200);
    EXPECT_TRUE(reset_called);
    EXPECT_NE(reset.body.find(R"("apSsid":"HomeDeck-A1B2C3")"), std::string::npos);
}

TEST_F(WifiRoutesTest, ResetReturns500WhenTheUnderlyingResetFails) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWifiRoutes(server, *auth_, []() -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto reset = HttpRequestRaw(server.BoundPort(), "POST", "/api/wifi/reset", "", cookie);
    EXPECT_EQ(reset.status_code, 500);
    EXPECT_NE(reset.body.find("reset_failed"), std::string::npos);
}
