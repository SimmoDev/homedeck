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

class AdminAuthRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_admin_auth_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    homedeck::HostTimeSource time_source_;
    std::unique_ptr<homedeck::AdminAuthService> auth_;
};

}  // namespace

TEST_F(AdminAuthRoutesTest, FullSetupLoginProtectedRouteLogoutFlow) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/api/test/protected",
                            auth_->RequireAuth([](const homedeck::HttpRequest&) {
                                return homedeck::HttpResponse{200, "text/plain", "secret data", {}};
                            }));
    ASSERT_TRUE(server.Start(18190));

    // 1. Status before setup: no password, not authenticated.
    auto status1 = HttpRequestRaw(18190, "GET", "/api/auth/status", "");
    EXPECT_EQ(status1.status_code, 200);
    EXPECT_NE(status1.body.find("\"passwordSet\":false"), std::string::npos);

    // 2. The protected route refuses before setup, with the specific
    // "setup required" signal, not a generic 401.
    auto protected_before_setup = HttpRequestRaw(18190, "GET", "/api/test/protected", "");
    EXPECT_EQ(protected_before_setup.status_code, 403);

    // 3. Setup succeeds and returns a session cookie.
    auto setup = HttpRequestRaw(18190, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    EXPECT_EQ(setup.status_code, 200);
    ASSERT_FALSE(setup.set_cookie.empty());
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    // 4. A second setup attempt is rejected now that one exists.
    auto second_setup = HttpRequestRaw(18190, "POST", "/api/auth/setup", R"({"password":"something else"})");
    EXPECT_EQ(second_setup.status_code, 409);

    // 5. The session from setup already works against the protected route.
    auto protected_with_session = HttpRequestRaw(18190, "GET", "/api/test/protected", "", cookie);
    EXPECT_EQ(protected_with_session.status_code, 200);
    EXPECT_EQ(protected_with_session.body, "secret data");

    // 6. Logging in fresh (a separate session) also works against the
    // now-set password.
    auto login = HttpRequestRaw(18190, "POST", "/api/auth/login", R"({"password":"correct horse battery"})");
    EXPECT_EQ(login.status_code, 200);
    ASSERT_FALSE(login.set_cookie.empty());
    std::string login_cookie = SessionCookieOnly(login.set_cookie);
    auto protected_with_login_session = HttpRequestRaw(18190, "GET", "/api/test/protected", "", login_cookie);
    EXPECT_EQ(protected_with_login_session.status_code, 200);

    // 7. Wrong password on login is rejected.
    auto bad_login = HttpRequestRaw(18190, "POST", "/api/auth/login", R"({"password":"wrong"})");
    EXPECT_EQ(bad_login.status_code, 401);

    // 8. Logout invalidates the session - the same cookie no longer
    // reaches the protected route.
    auto logout = HttpRequestRaw(18190, "POST", "/api/auth/logout", "", cookie);
    EXPECT_EQ(logout.status_code, 200);
    auto protected_after_logout = HttpRequestRaw(18190, "GET", "/api/test/protected", "", cookie);
    EXPECT_EQ(protected_after_logout.status_code, 401);
}

// Brute-force throttle (AdminAuthService::kMaxFailedLoginAttempts) -
// recovery-after-lockout itself is covered at the service level
// (admin_auth_service_test.cpp, with a fake clock); this confirms the
// route layer surfaces it as a distinct 429, not just another 401.
TEST_F(AdminAuthRoutesTest, LoginIsThrottledAfterRepeatedFailures) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    ASSERT_TRUE(server.Start(18191));

    auto setup = HttpRequestRaw(18191, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    for (int i = 0; i < 5; i++) {
        auto bad_login = HttpRequestRaw(18191, "POST", "/api/auth/login", R"({"password":"wrong"})");
        EXPECT_EQ(bad_login.status_code, 401);
    }

    // Locked out now - even the correct password gets 429, not 200.
    auto locked_out = HttpRequestRaw(18191, "POST", "/api/auth/login", R"({"password":"correct horse battery"})");
    EXPECT_EQ(locked_out.status_code, 429);
    EXPECT_NE(locked_out.body.find("too_many_attempts"), std::string::npos);
}
