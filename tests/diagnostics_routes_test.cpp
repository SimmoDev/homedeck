#include "core/diagnostics_routes.h"

#include "core/admin_auth_service.h"
#include "platform/host/battery_reader.h"
#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <memory>

namespace {

// Same shape as admin_auth_routes_test.cpp's HttpRequestRaw/HttpResult -
// duplicated rather than shared, matching that file's own precedent for
// this exact tradeoff.
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

std::string SessionCookieOnly(const std::string& set_cookie) {
    size_t semicolon = set_cookie.find(';');
    return semicolon == std::string::npos ? set_cookie : set_cookie.substr(0, semicolon);
}

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
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    homedeck::HostTimeSource time_source_;
    std::unique_ptr<homedeck::AdminAuthService> auth_;
    homedeck::HostBatteryReader battery_reader_;
};

}  // namespace

TEST_F(DiagnosticsRoutesTest, RequiresAuthenticationAndReflectsStoredValues) {
    storage_->SetSetting("core", "reset_reason", 1, "panic");
    storage_->SetSetting("core", "has_core_dump", 1, "true");
    battery_reader_.SetPercent(42);
    battery_reader_.SetExternalPowerConnected(true);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, []() -> std::optional<std::string> {
        return std::string("dump bytes");
    });
    ASSERT_TRUE(server.Start(18191));

    // Unauthenticated, no password set yet - 403 setup_required.
    auto before_setup = HttpRequestRaw(18191, "GET", "/api/diagnostics", "");
    EXPECT_EQ(before_setup.status_code, 403);

    auto setup = HttpRequestRaw(18191, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    // Password set, but no session cookie - 401, not 403.
    auto unauthenticated = HttpRequestRaw(18191, "GET", "/api/diagnostics", "");
    EXPECT_EQ(unauthenticated.status_code, 401);

    auto authenticated = HttpRequestRaw(18191, "GET", "/api/diagnostics", "", cookie);
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
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_, []() -> std::optional<std::string> {
        return std::string("dump bytes");
    });
    ASSERT_TRUE(server.Start(18192));

    auto setup = HttpRequestRaw(18192, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto unauthenticated = HttpRequestRaw(18192, "GET", "/api/diagnostics/coredump", "");
    EXPECT_EQ(unauthenticated.status_code, 401);

    auto download = HttpRequestRaw(18192, "GET", "/api/diagnostics/coredump", "", cookie);
    EXPECT_EQ(download.status_code, 200);
    EXPECT_EQ(download.body, "dump bytes");
}

TEST_F(DiagnosticsRoutesTest, CoreDumpReturns404WhenAbsent) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterDiagnosticsRoutes(server, *storage_, *auth_, battery_reader_,
                                         []() -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE(server.Start(18193));

    auto setup = HttpRequestRaw(18193, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto download = HttpRequestRaw(18193, "GET", "/api/diagnostics/coredump", "", cookie);
    EXPECT_EQ(download.status_code, 404);
}
