#include "core/settings_routes.h"

#include "core/admin_auth_service.h"
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

// Same shape as diagnostics_routes_test.cpp's/ota_routes_test.cpp's
// HttpRequestRaw/HttpResult - duplicated rather than shared, matching
// those files' own precedent for this exact tradeoff.
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

    size_t total_sent = 0;
    while (total_sent < request.size()) {
        ssize_t sent = send(sock, request.data() + total_sent, request.size() - total_sent, 0);
        if (sent <= 0) {
            break;
        }
        total_sent += static_cast<size_t>(sent);
    }

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

class SettingsRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_settings_routes_test_" +
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
    // RegisterAdminAuthRoutes) before calling Start(). The real
    // password hash this writes is exactly what the reserved-key tests
    // below confirm stays unclobbered.
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

TEST_F(SettingsRoutesTest, AllFiveEndpointsRequireAuthentication) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18210));

    EXPECT_EQ(HttpRequestRaw(18210, "GET", "/api/settings", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/settings", "{}").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/settings/erase", "{}").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18210, "GET", "/api/backup", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/backup/restore", "{}").status_code, 403);

    auto setup = HttpRequestRaw(18210, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    EXPECT_EQ(HttpRequestRaw(18210, "GET", "/api/settings", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/settings", "{}").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/settings/erase", "{}").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18210, "GET", "/api/backup", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18210, "POST", "/api/backup/restore", "{}").status_code, 401);
}

TEST_F(SettingsRoutesTest, SetThenGetRoundTrips) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18211));
    std::string cookie = Login(18211);

    auto post = HttpRequestRaw(18211, "POST", "/api/settings",
                                R"({"module":"harmony","key":"hub_ip","value":"10.0.0.5","schemaVersion":1})",
                                cookie);
    EXPECT_EQ(post.status_code, 200);

    auto get = HttpRequestRaw(18211, "GET", "/api/settings", "", cookie);
    EXPECT_EQ(get.status_code, 200);
    EXPECT_NE(get.body.find(R"("module":"harmony")"), std::string::npos);
    EXPECT_NE(get.body.find(R"("key":"hub_ip")"), std::string::npos);
    EXPECT_NE(get.body.find(R"("value":"10.0.0.5")"), std::string::npos);
    EXPECT_NE(get.body.find(R"("schemaVersion":1)"), std::string::npos);
}

TEST_F(SettingsRoutesTest, PostSettingsRejectsMissingFields) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18212));
    std::string cookie = Login(18212);

    auto missing_key = HttpRequestRaw(18212, "POST", "/api/settings",
                                       R"({"module":"harmony","value":"x","schemaVersion":1})", cookie);
    EXPECT_EQ(missing_key.status_code, 400);
    EXPECT_NE(missing_key.body.find(R"("field":"key")"), std::string::npos);
}

TEST_F(SettingsRoutesTest, PostSettingsRejectsOversizedModuleOrKey) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18213));
    std::string cookie = Login(18213);

    auto post = HttpRequestRaw(
        18213, "POST", "/api/settings",
        R"({"module":"harmony","key":"this_key_is_way_too_long_for_nvs","value":"x","schemaVersion":1})", cookie);
    EXPECT_EQ(post.status_code, 400);
    EXPECT_NE(post.body.find("invalid_key"), std::string::npos);
}

TEST_F(SettingsRoutesTest, PostSettingsRejectsPathTraversalInModuleOrKey) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18218));
    std::string cookie = Login(18218);

    auto post = HttpRequestRaw(18218, "POST", "/api/settings",
                                R"({"module":"..","key":"../../secrets","value":"x","schemaVersion":1})", cookie);
    EXPECT_EQ(post.status_code, 400);
    EXPECT_NE(post.body.find("invalid_key"), std::string::npos);

    // Confirms the rejection is real, not just a satisfied response code -
    // nothing escaped root_dir_ onto disk.
    EXPECT_FALSE(std::filesystem::exists(root_dir_.parent_path() / "secrets"));
}

TEST_F(SettingsRoutesTest, EraseRemovesAKey) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18214));
    std::string cookie = Login(18214);

    HttpRequestRaw(18214, "POST", "/api/settings",
                    R"({"module":"harmony","key":"hub_ip","value":"10.0.0.5","schemaVersion":1})", cookie);
    auto erase =
        HttpRequestRaw(18214, "POST", "/api/settings/erase", R"({"module":"harmony","key":"hub_ip"})", cookie);
    EXPECT_EQ(erase.status_code, 200);

    auto get = HttpRequestRaw(18214, "GET", "/api/settings", "", cookie);
    EXPECT_EQ(get.body, "[]");
}

// Regression test for the shared-NVS-namespace finding
// (docs/decisions/ADR-0023-settings-backup-api.md): SettingsStore and
// SecretStore write into the same physical NVS namespace-per-module on
// firmware, so a fully generic settings write/list could otherwise read
// or overwrite the admin password hash through the wrong door.
TEST_F(SettingsRoutesTest, ReservedAdminPasswordKeyIsRejectedAndNeverExposed) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18215));
    std::string cookie = Login(18215);

    // The real password hash Login() just set, via the legitimate
    // SecretStore path - this must be unchanged by everything below.
    auto original_hash =
        storage_->GetSecret(homedeck::AdminAuthService::kModuleId, homedeck::AdminAuthService::kPasswordKey);
    ASSERT_TRUE(original_hash.has_value());

    auto post =
        HttpRequestRaw(18215, "POST", "/api/settings",
                        R"({"module":"core","key":"admin_pw_hash","value":"clobbered","schemaVersion":1})", cookie);
    EXPECT_NE(post.status_code, 200);

    auto get_settings = HttpRequestRaw(18215, "GET", "/api/settings", "", cookie);
    EXPECT_EQ(get_settings.body.find("admin_pw_hash"), std::string::npos);

    auto get_backup = HttpRequestRaw(18215, "GET", "/api/backup", "", cookie);
    EXPECT_EQ(get_backup.body.find("admin_pw_hash"), std::string::npos);

    auto restore = HttpRequestRaw(
        18215, "POST", "/api/backup/restore",
        R"({"settings":[{"module":"core","key":"admin_pw_hash","value":"clobbered-via-restore","schemaVersion":1}]})",
        cookie);
    EXPECT_EQ(restore.status_code, 200);
    EXPECT_NE(restore.body.find(R"("applied":0)"), std::string::npos);

    auto hash_after =
        storage_->GetSecret(homedeck::AdminAuthService::kModuleId, homedeck::AdminAuthService::kPasswordKey);
    ASSERT_TRUE(hash_after.has_value());
    EXPECT_EQ(hash_after->value, original_hash->value);
}

TEST_F(SettingsRoutesTest, RestoreReplaysEntriesAndReportsFailures) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18216));
    std::string cookie = Login(18216);

    auto restore =
        HttpRequestRaw(18216, "POST", "/api/backup/restore",
                        R"({"settings":[{"module":"harmony","key":"hub_ip","value":"10.0.0.5","schemaVersion":1}]})",
                        cookie);
    EXPECT_EQ(restore.status_code, 200);
    EXPECT_NE(restore.body.find(R"("applied":1)"), std::string::npos);

    auto get = HttpRequestRaw(18216, "GET", "/api/settings", "", cookie);
    EXPECT_NE(get.body.find(R"("value":"10.0.0.5")"), std::string::npos);
}

TEST_F(SettingsRoutesTest, DeviceNameChangedCallbackCanAcceptOrReject) {
    std::vector<std::string> applied_values;
    homedeck::DeviceNameChangedFn on_device_name_changed = [&applied_values](const std::string& value) -> bool {
        if (value == "reject-me") return false;
        applied_values.push_back(value);
        return true;
    };

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_, on_device_name_changed);
    ASSERT_TRUE(server.Start(18217));
    std::string cookie = Login(18217);

    auto accepted = HttpRequestRaw(
        18217, "POST", "/api/settings",
        R"({"module":"core","key":"device_name","value":"living-room","schemaVersion":1})", cookie);
    EXPECT_EQ(accepted.status_code, 200);
    ASSERT_EQ(applied_values.size(), 1u);
    EXPECT_EQ(applied_values[0], "living-room");

    auto rejected =
        HttpRequestRaw(18217, "POST", "/api/settings",
                        R"({"module":"core","key":"device_name","value":"reject-me","schemaVersion":1})", cookie);
    EXPECT_EQ(rejected.status_code, 400);

    // The rejected value must not have been persisted - the accepted
    // one from before should still be the current value.
    auto get = HttpRequestRaw(18217, "GET", "/api/settings", "", cookie);
    EXPECT_NE(get.body.find(R"("value":"living-room")"), std::string::npos);
    EXPECT_EQ(get.body.find("reject-me"), std::string::npos);
}
