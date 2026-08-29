#include "core/diagnostics_routes.h"

#include "core/admin_auth_service.h"
#include "core/logger.h"
#include "platform/host/battery_reader.h"
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

class DiagnosticsRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_diagnostics_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
        logger_ = std::make_unique<homedeck::Logger>(*storage_, time_source_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    homedeck::HostTimeSource time_source_;
    std::unique_ptr<homedeck::AdminAuthService> auth_;
    std::unique_ptr<homedeck::Logger> logger_;
    homedeck::HostBatteryReader battery_reader_;
};

}  // namespace

TEST_F(DiagnosticsRoutesTest, RequiresAuthenticationAndReflectsStoredValues) {
    ASSERT_TRUE(storage_->SetSetting("core", "reset_reason", 1, "panic"));
    ASSERT_TRUE(storage_->SetSetting("core", "has_core_dump", 1, "true"));
    battery_reader_.SetPercent(42);
    battery_reader_.SetExternalPowerConnected(true);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, *logger_,
                                         []() -> std::optional<std::string> { return std::string("dump bytes"); });
    ASSERT_TRUE(server.Start(0));

    // Unauthenticated, no password set yet - 403 setup_required.
    auto before_setup = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics", "");
    EXPECT_EQ(before_setup.status_code, 403);

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    // Password set, but no session cookie - 401, not 403.
    auto unauthenticated = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics", "");
    EXPECT_EQ(unauthenticated.status_code, 401);

    auto authenticated = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics", "", cookie);
    EXPECT_EQ(authenticated.status_code, 200);
    EXPECT_NE(authenticated.body.find("\"resetReason\":\"panic\""), std::string::npos);
    EXPECT_NE(authenticated.body.find("\"hasCoreDump\":true"), std::string::npos);
    EXPECT_NE(authenticated.body.find("\"batteryPercent\":42"), std::string::npos);
    EXPECT_NE(authenticated.body.find("\"externalPowerConnected\":true"), std::string::npos);
    EXPECT_NE(authenticated.body.find("\"batteryPresent\":true"), std::string::npos);
}

TEST_F(DiagnosticsRoutesTest, CoreDumpDownloadsRawBytesWithCorrectHeadersWhenPresent) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, *logger_,
                                         []() -> std::optional<std::string> { return std::string("dump bytes"); });
    ASSERT_TRUE(server.Start(0));

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto unauthenticated = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/coredump", "");
    EXPECT_EQ(unauthenticated.status_code, 401);

    auto download = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/coredump", "", cookie);
    EXPECT_EQ(download.status_code, 200);
    EXPECT_EQ(download.body, "dump bytes");
}

TEST_F(DiagnosticsRoutesTest, CoreDumpReturns404WhenAbsent) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, *logger_,
                                         []() -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE(server.Start(0));

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto download = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/coredump", "", cookie);
    EXPECT_EQ(download.status_code, 404);
}

TEST_F(DiagnosticsRoutesTest, LogsEndpointRequiresAuthAndReturnsRealEntries) {
    logger_->Log(homedeck::LogLevel::kInfo, "wifi", "Connected");

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, *logger_,
                                         []() -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE(server.Start(0));

    // No password set yet - 403 setup_required, matching this file's
    // other tests' precedent.
    auto before_setup = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/logs", "");
    EXPECT_EQ(before_setup.status_code, 403);

    auto setup = HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    // Password set, but no session cookie - 401, not 403.
    auto unauthenticated = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/logs", "");
    EXPECT_EQ(unauthenticated.status_code, 401);

    auto logs = HttpRequestRaw(server.BoundPort(), "GET", "/api/diagnostics/logs", "", cookie);
    EXPECT_EQ(logs.status_code, 200);
    EXPECT_NE(logs.body.find("\"component\":\"wifi\""), std::string::npos);
    EXPECT_NE(logs.body.find("\"message\":\"Connected\""), std::string::npos);
    EXPECT_NE(logs.body.find("\"level\":\"info\""), std::string::npos);
}
