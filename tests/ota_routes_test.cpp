#include "core/ota_routes.h"

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
#include <vector>

namespace {

// Same shape as diagnostics_routes_test.cpp's HttpRequestRaw/HttpResult -
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

class OtaRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_ota_routes_test_" +
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
    // contract; callers register their own routes (and RegisterAdminAuthRoutes)
    // before calling Start().
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
    homedeck::HostBatteryReader battery_reader_;
    homedeck::EventBus event_bus_;
};

homedeck::OtaWriter MakeWriter(size_t max_size, bool write_should_succeed, std::string* written_image = nullptr) {
    return homedeck::OtaWriter{
        .max_image_size = [max_size]() -> size_t { return max_size; },
        .write_image =
            [write_should_succeed, written_image](const std::string& image) -> bool {
                if (written_image != nullptr) {
                    *written_image = image;
                }
                return write_should_succeed;
            },
        .running_version = []() -> std::string { return "1.2.3"; },
    };
}

}  // namespace

TEST_F(OtaRoutesTest, AllThreeEndpointsRequireAuthentication) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    bool rebooted = false;
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true),
                                 [&rebooted]() { rebooted = true; });
    ASSERT_TRUE(server.Start(18201));

    // No password set yet - 403 setup_required, matching diagnostics_routes_test.cpp's precedent.
    EXPECT_EQ(HttpRequestRaw(18201, "GET", "/api/ota/status", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18201, "POST", "/api/ota/upload", "x").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18201, "POST", "/api/ota/reboot", "").status_code, 403);

    auto setup = HttpRequestRaw(18201, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    // Password set, but no session cookie - 401.
    EXPECT_EQ(HttpRequestRaw(18201, "GET", "/api/ota/status", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18201, "POST", "/api/ota/upload", "x").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18201, "POST", "/api/ota/reboot", "").status_code, 401);
    EXPECT_FALSE(rebooted);
}

TEST_F(OtaRoutesTest, StatusReflectsBatteryGateAndVersion) {
    battery_reader_.SetPercent(50);
    battery_reader_.SetExternalPowerConnected(false);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true), []() {});
    ASSERT_TRUE(server.Start(18202));
    std::string cookie = Login(18202);

    auto status = HttpRequestRaw(18202, "GET", "/api/ota/status", "", cookie);
    EXPECT_EQ(status.status_code, 200);
    EXPECT_NE(status.body.find("\"batteryPercent\":50"), std::string::npos);
    EXPECT_NE(status.body.find("\"externalPowerConnected\":false"), std::string::npos);
    EXPECT_NE(status.body.find("\"gateOpen\":true"), std::string::npos);
    EXPECT_NE(status.body.find("\"runningVersion\":\"1.2.3\""), std::string::npos);
}

TEST_F(OtaRoutesTest, UploadRejectedWithReasonWhenGateClosed) {
    battery_reader_.SetPercent(5);
    battery_reader_.SetExternalPowerConnected(false);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    std::string written_image;
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_,
                                 MakeWriter(1024, true, &written_image), []() {});
    ASSERT_TRUE(server.Start(18203));
    std::string cookie = Login(18203);

    auto upload = HttpRequestRaw(18203, "POST", "/api/ota/upload", "firmware bytes", cookie);
    EXPECT_EQ(upload.status_code, 403);
    EXPECT_NE(upload.body.find("gate_closed"), std::string::npos);
    EXPECT_TRUE(written_image.empty());
}

TEST_F(OtaRoutesTest, UploadRejectedWhenLargerThanMaxImageSize) {
    battery_reader_.SetPercent(100);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    std::string written_image;
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_,
                                 MakeWriter(4, true, &written_image), []() {});
    ASSERT_TRUE(server.Start(18204));
    std::string cookie = Login(18204);

    auto upload = HttpRequestRaw(18204, "POST", "/api/ota/upload", "this image is too large", cookie);
    EXPECT_EQ(upload.status_code, 400);
    EXPECT_TRUE(written_image.empty());
}

TEST_F(OtaRoutesTest, UploadSucceedsAgainstASmallFakeImageWhenGateOpen) {
    battery_reader_.SetPercent(100);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    std::string written_image;
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_,
                                 MakeWriter(1024, true, &written_image), []() {});
    ASSERT_TRUE(server.Start(18205));
    std::string cookie = Login(18205);

    auto upload = HttpRequestRaw(18205, "POST", "/api/ota/upload", "fake firmware image bytes", cookie);
    EXPECT_EQ(upload.status_code, 200);
    EXPECT_EQ(written_image, "fake firmware image bytes");
}

TEST_F(OtaRoutesTest, UploadReturns500WhenWriteFails) {
    battery_reader_.SetPercent(100);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, false), []() {});
    ASSERT_TRUE(server.Start(18206));
    std::string cookie = Login(18206);

    auto upload = HttpRequestRaw(18206, "POST", "/api/ota/upload", "fake firmware image bytes", cookie);
    EXPECT_EQ(upload.status_code, 500);
}

TEST_F(OtaRoutesTest, ExternalPowerOpensGateEvenAtLowBattery) {
    battery_reader_.SetPercent(5);
    battery_reader_.SetExternalPowerConnected(true);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true), []() {});
    ASSERT_TRUE(server.Start(18207));
    std::string cookie = Login(18207);

    auto status = HttpRequestRaw(18207, "GET", "/api/ota/status", "", cookie);
    EXPECT_NE(status.body.find("\"gateOpen\":true"), std::string::npos);

    auto upload = HttpRequestRaw(18207, "POST", "/api/ota/upload", "fake firmware image bytes", cookie);
    EXPECT_EQ(upload.status_code, 200);
}

TEST_F(OtaRoutesTest, RebootCallsTheInjectedRebootFunction) {
    battery_reader_.SetPercent(100);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    bool rebooted = false;
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true),
                                 [&rebooted]() { rebooted = true; });
    ASSERT_TRUE(server.Start(18208));
    std::string cookie = Login(18208);

    auto reboot = HttpRequestRaw(18208, "POST", "/api/ota/reboot", "", cookie);
    EXPECT_EQ(reboot.status_code, 200);
    EXPECT_TRUE(rebooted);
}

TEST_F(OtaRoutesTest, SuccessfulUploadPublishesInProgressThenFinished) {
    battery_reader_.SetPercent(100);

    std::vector<bool> events;
    auto sub = event_bus_.Subscribe<homedeck::OtaUpdateStateChangedEvent>(
        [&events](const homedeck::OtaUpdateStateChangedEvent& event) { events.push_back(event.in_progress); });

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true), []() {});
    ASSERT_TRUE(server.Start(18209));
    std::string cookie = Login(18209);

    auto upload = HttpRequestRaw(18209, "POST", "/api/ota/upload", "fake firmware image bytes", cookie);
    EXPECT_EQ(upload.status_code, 200);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(events[0]);
    EXPECT_FALSE(events[1]);
}

TEST_F(OtaRoutesTest, FailedWriteStillPublishesFinishedNotLeftStuckInProgress) {
    battery_reader_.SetPercent(100);

    std::vector<bool> events;
    auto sub = event_bus_.Subscribe<homedeck::OtaUpdateStateChangedEvent>(
        [&events](const homedeck::OtaUpdateStateChangedEvent& event) { events.push_back(event.in_progress); });

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, false), []() {});
    ASSERT_TRUE(server.Start(18210));
    std::string cookie = Login(18210);

    auto upload = HttpRequestRaw(18210, "POST", "/api/ota/upload", "fake firmware image bytes", cookie);
    EXPECT_EQ(upload.status_code, 500);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(events[0]);
    EXPECT_FALSE(events[1]);
}

TEST_F(OtaRoutesTest, GateClosedRejectionPublishesNeitherEvent) {
    battery_reader_.SetPercent(5);
    battery_reader_.SetExternalPowerConnected(false);

    int event_count = 0;
    auto sub = event_bus_.Subscribe<homedeck::OtaUpdateStateChangedEvent>(
        [&event_count](const homedeck::OtaUpdateStateChangedEvent&) { event_count++; });

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterOtaRoutes(server, event_bus_, *auth_, battery_reader_, MakeWriter(1024, true), []() {});
    ASSERT_TRUE(server.Start(18211));
    std::string cookie = Login(18211);

    auto upload = HttpRequestRaw(18211, "POST", "/api/ota/upload", "firmware bytes", cookie);
    EXPECT_EQ(upload.status_code, 403);
    EXPECT_EQ(event_count, 0);
}
