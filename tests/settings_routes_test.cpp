#include "core/settings_routes.h"

#include "core/admin_auth_service.h"
#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"
#include "third_party/nlohmann/json.hpp"

#include "http_test_helpers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

namespace {

using homedeck::testing::HttpRequestRaw;
using homedeck::testing::HttpResult;
using homedeck::testing::SessionCookieOnly;

// Always fails Set() - simulates a genuine storage write failure (e.g.
// NVS full). Real SettingsStore backends don't fail like this in a
// test-controllable way, hence the fake - same precedent as
// admin_auth_routes_test.cpp's FailingSecretStore.
class FailingSettingsStore : public homedeck::SettingsStore {
public:
    bool Set(const std::string&, const std::string&, const std::string&) override { return false; }
    std::optional<std::string> Get(const std::string&, const std::string&) override { return std::nullopt; }
    bool Erase(const std::string&, const std::string&) override { return false; }
    std::vector<homedeck::SettingsEntry> ListAll() override { return {}; }
};

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

TEST_F(SettingsRoutesTest, PostSettingsRejectsAValueOverTheLengthCap) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterSettingsRoutes(server, *storage_, *auth_);
    ASSERT_TRUE(server.Start(18220));
    std::string cookie = Login(18220);

    // kMaxSettingValueLength (4096, settings_routes.cpp) - one byte over.
    std::string oversized_value(4097, 'x');
    nlohmann::json body = {{"module", "harmony"}, {"key", "note"}, {"value", oversized_value}, {"schemaVersion", 1}};
    auto post = HttpRequestRaw(18220, "POST", "/api/settings", body.dump(), cookie);
    EXPECT_EQ(post.status_code, 400);
    EXPECT_NE(post.body.find("value_too_long"), std::string::npos);

    EXPECT_FALSE(storage_->GetSetting("harmony", "note").has_value());
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
    EXPECT_EQ(post.status_code, 403);
    EXPECT_NE(post.body.find("reserved_key"), std::string::npos);

    auto erase =
        HttpRequestRaw(18215, "POST", "/api/settings/erase", R"({"module":"core","key":"admin_pw_hash"})", cookie);
    EXPECT_EQ(erase.status_code, 403);
    EXPECT_NE(erase.body.find("reserved_key"), std::string::npos);

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
    // Reported as rejected, not failed - distinguishable from a genuine
    // storage fault, same as the direct POST/erase 403s above.
    auto rejected_pos = restore.body.find(R"("rejected":)");
    ASSERT_NE(rejected_pos, std::string::npos);
    EXPECT_NE(restore.body.find("admin_pw_hash", rejected_pos), std::string::npos);
    EXPECT_NE(restore.body.find(R"("failed":[])"), std::string::npos);

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

TEST_F(SettingsRoutesTest, PostSettingsReturns500WhenTheStorageWriteFails) {
    FailingSettingsStore failing_settings_store;
    homedeck::HostCacheStore cache_store(root_dir_ / "failing_case");
    homedeck::HostSecretStore secret_store(root_dir_ / "failing_case");
    homedeck::Storage failing_storage(failing_settings_store, cache_store, secret_store);
    homedeck::AdminAuthService failing_auth(failing_storage, time_source_);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, failing_auth);
    homedeck::RegisterSettingsRoutes(server, failing_storage, failing_auth);
    ASSERT_TRUE(server.Start(18219));
    auto setup = HttpRequestRaw(18219, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto result = HttpRequestRaw(18219, "POST", "/api/settings",
                                  R"({"module":"harmony","key":"hub_ip","value":"10.0.0.5","schemaVersion":1})",
                                  cookie);
    EXPECT_EQ(result.status_code, 500);
    EXPECT_NE(result.body.find("write_failed"), std::string::npos);
}

TEST_F(SettingsRoutesTest, PostSettingsEraseReturns500WhenTheStorageEraseFails) {
    FailingSettingsStore failing_settings_store;
    homedeck::HostCacheStore cache_store(root_dir_ / "failing_erase_case");
    homedeck::HostSecretStore secret_store(root_dir_ / "failing_erase_case");
    homedeck::Storage failing_storage(failing_settings_store, cache_store, secret_store);
    homedeck::AdminAuthService failing_auth(failing_storage, time_source_);

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, failing_auth);
    homedeck::RegisterSettingsRoutes(server, failing_storage, failing_auth);
    ASSERT_TRUE(server.Start(18220));
    auto setup = HttpRequestRaw(18220, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    std::string cookie = SessionCookieOnly(setup.set_cookie);

    auto result =
        HttpRequestRaw(18220, "POST", "/api/settings/erase", R"({"module":"harmony","key":"hub_ip"})", cookie);
    EXPECT_EQ(result.status_code, 500);
    EXPECT_NE(result.body.find("erase_failed"), std::string::npos);
}
