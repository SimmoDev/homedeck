#include "core/admin_auth_service.h"

#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

namespace {

// A minimal real HTTP/1.1 client over a raw socket, extended from
// http_server_test.cpp's HttpGet to support POST bodies and to hand
// back the response headers - AdminAuthService's routes need both
// (setting/reading the session cookie), where the existing helper's
// return-the-whole-response-as-one-string approach isn't enough.
struct HttpResult {
    int status_code = 0;
    std::string set_cookie;
    std::string body;
};

HttpResult HttpRequestRaw(uint16_t port, const std::string& method, const std::string& path,
                           const std::string& body, const std::string& cookie_header = "") {
    HttpResult result;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return result;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return result;
    }

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!cookie_header.empty()) {
        request += "Cookie: " + cookie_header + "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n" + body;
    send(sock, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<size_t>(received));
    }
    close(sock);

    size_t header_end = response.find("\r\n\r\n");
    std::string headers = header_end == std::string::npos ? response : response.substr(0, header_end);
    result.body = header_end == std::string::npos ? "" : response.substr(header_end + 4);

    size_t status_start = headers.find(' ');
    if (status_start != std::string::npos) {
        result.status_code = std::atoi(headers.c_str() + status_start + 1);
    }
    size_t cookie_pos = headers.find("Set-Cookie: ");
    if (cookie_pos != std::string::npos) {
        size_t value_start = cookie_pos + std::strlen("Set-Cookie: ");
        size_t line_end = headers.find("\r\n", value_start);
        result.set_cookie = headers.substr(value_start, line_end - value_start);
    }
    return result;
}

class AdminAuthRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_admin_auth_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    // Extracts just the "session=<token>" part of a Set-Cookie value
    // (which also carries HttpOnly/SameSite/Path/Max-Age) for use as a
    // request's Cookie header.
    static std::string SessionCookieOnly(const std::string& set_cookie) {
        size_t semicolon = set_cookie.find(';');
        return semicolon == std::string::npos ? set_cookie : set_cookie.substr(0, semicolon);
    }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
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
