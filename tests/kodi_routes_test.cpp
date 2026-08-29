#include "core/kodi_routes.h"

#include "core/admin_auth_service.h"
#include "core/kodi_client.h"
#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"
#include "platform/mdns_browser.h"
#include "third_party/nlohmann/json.hpp"

#include "http_test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace {

using homedeck::testing::HttpRequestRaw;

template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 300) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class NoInstancesMdnsBrowser : public homedeck::MdnsBrowser {
public:
    std::vector<homedeck::MdnsService> Browse(const std::string&, std::chrono::milliseconds) override { return {}; }
};

// KodiClient needs a WebSocketClient factory even for tests that never
// configure a host and so never connect.
class UnusedWebSocketClient : public homedeck::WebSocketClient {
public:
    bool Connect(const std::string&) override { return false; }
    bool SendText(const std::string&) override { return false; }
    std::optional<std::string> ReceiveText(int) override { return std::nullopt; }
    void Close() override {}
};

// Answers KodiClient's reconcile-poll JSON-RPC by echoing each request's
// id - a self-contained stateful double (one in-flight request at a
// time, which is all KodiClient::Call() ever has). Stable across repeat
// polls so the connection keeps passing its own liveness check.
class ConnectedKodiWebSocketClient : public homedeck::WebSocketClient {
public:
    bool Connect(const std::string&) override { return true; }
    bool SendText(const std::string& text) override {
        nlohmann::json request = nlohmann::json::parse(text, nullptr, false);
        if (request.is_object()) {
            last_id_ = request.value("id", nlohmann::json(0));
            last_method_ = request.value("method", "");
        }
        return true;
    }
    std::optional<std::string> ReceiveText(int timeout_ms) override {
        if (timeout_ms == 0 || last_method_.empty()) {
            return std::nullopt;
        }
        nlohmann::json result;
        if (last_method_ == "Application.GetProperties") {
            result = {{"volume", 55}, {"muted", false}, {"version", {{"major", 21}, {"minor", 2}}}};
        } else if (last_method_ == "Player.GetActivePlayers") {
            result = nlohmann::json::array();
        } else {
            result = "OK";
        }
        nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", last_id_}, {"result", result}};
        last_method_.clear();
        return reply.dump();
    }
    void Close() override {}

private:
    nlohmann::json last_id_ = 0;
    std::string last_method_;
};

class KodiRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_kodi_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
        event_bus_ = std::make_unique<homedeck::EventBus>();
    }
    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::unique_ptr<homedeck::KodiClient> MakeClient(homedeck::KodiClient::WebSocketClientFactory factory) {
        return std::make_unique<homedeck::KodiClient>(std::move(factory), browser_, *storage_, *event_bus_);
    }

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
    NoInstancesMdnsBrowser browser_;
};

}  // namespace

TEST_F(KodiRoutesTest, BothEndpointsRequireAuthentication) {
    auto client = MakeClient([] { return std::make_unique<UnusedWebSocketClient>(); });
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterKodiRoutes(server, *client, *auth_);
    ASSERT_TRUE(server.Start(0));

    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "GET", "/api/kodi/status", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/kodi/reconnect", "").status_code, 403);

    ASSERT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/auth/setup", R"({"password":"correct horse battery"})").status_code,
              200);

    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "GET", "/api/kodi/status", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/kodi/reconnect", "").status_code, 401);
}

TEST_F(KodiRoutesTest, StatusReportsDisconnectedBeforeAnyInstanceIsConfigured) {
    auto client = MakeClient([] { return std::make_unique<UnusedWebSocketClient>(); });
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterKodiRoutes(server, *client, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto result = HttpRequestRaw(server.BoundPort(), "GET", "/api/kodi/status", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("state":"disconnected")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("hasStatus":false)"), std::string::npos);
    EXPECT_NE(result.body.find(R"("discovered":[])"), std::string::npos);
}

TEST_F(KodiRoutesTest, StatusReportsPopulatedSnapshotOnceConnected) {
    ASSERT_TRUE(storage_->SetSetting(homedeck::KodiClient::kModuleId, homedeck::KodiClient::kHostKey, 1, "127.0.0.1"));
    auto client = MakeClient([] { return std::make_unique<ConnectedKodiWebSocketClient>(); });
    client->Start();
    ASSERT_TRUE(WaitFor([&] { return client->Snapshot().state == homedeck::KodiConnectionState::kConnected; }));

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterKodiRoutes(server, *client, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    auto result = HttpRequestRaw(server.BoundPort(), "GET", "/api/kodi/status", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("state":"connected")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("resolvedHost":"127.0.0.1")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("appVersion":"21.2")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("nowPlaying")"), std::string::npos);

    client->Stop();
}

TEST_F(KodiRoutesTest, ReconnectReturnsOk) {
    auto client = MakeClient([] { return std::make_unique<UnusedWebSocketClient>(); });
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterKodiRoutes(server, *client, *auth_);
    ASSERT_TRUE(server.Start(0));
    std::string cookie = Login(server.BoundPort());

    EXPECT_EQ(HttpRequestRaw(server.BoundPort(), "POST", "/api/kodi/reconnect", "", cookie).status_code, 200);
}
