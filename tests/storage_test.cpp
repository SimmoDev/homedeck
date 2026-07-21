#include "core/admin_auth_service.h"
#include "core/storage.h"
#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

class StorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_storage_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
};

}  // namespace

TEST_F(StorageTest, SettingRoundTrips) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));

    auto result = storage.GetSetting("harmony", "hub_ip");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->schema_version, 1);
    EXPECT_EQ(result->value, "10.0.0.5");
}

TEST_F(StorageTest, GetSettingOnMissingKeyReturnsNullopt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    EXPECT_FALSE(storage.GetSetting("harmony", "never_written").has_value());
}

TEST_F(StorageTest, SettingsAreNamespacedPerModule) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.SetSetting("kodi", "hub_ip", 1, "10.0.0.9"));

    EXPECT_EQ(storage.GetSetting("harmony", "hub_ip")->value, "10.0.0.5");
    EXPECT_EQ(storage.GetSetting("kodi", "hub_ip")->value, "10.0.0.9");
}

TEST_F(StorageTest, OverwritingASettingUpdatesBothVersionAndValue) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 2, "10.0.0.6"));

    auto result = storage.GetSetting("harmony", "hub_ip");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->schema_version, 2);
    EXPECT_EQ(result->value, "10.0.0.6");
}

TEST_F(StorageTest, EraseSettingRemovesIt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.EraseSetting("harmony", "hub_ip"));

    EXPECT_FALSE(storage.GetSetting("harmony", "hub_ip").has_value());
}

TEST_F(StorageTest, SecretRoundTripsAndIsSeparateFromSettings) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    // Not "admin_pw_hash" - that exact (module_id, key) is reserved (see
    // ReservedAdminPasswordKeyTests below), which would make SetSetting
    // fail here for a reason unrelated to what this test is actually
    // checking.
    ASSERT_TRUE(storage.SetSetting("core", "some_other_key", 1, "settings-value"));
    ASSERT_TRUE(storage.SetSecret("core", "admin_pw_hash", 1, "secret-value"));

    EXPECT_EQ(storage.GetSetting("core", "some_other_key")->value, "settings-value");
    EXPECT_EQ(storage.GetSecret("core", "admin_pw_hash")->value, "secret-value");
}

TEST_F(StorageTest, EraseSecretRemovesIt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSecret("core", "admin_pw_hash", 1, "secret-value"));
    ASSERT_TRUE(storage.EraseSecret("core", "admin_pw_hash"));

    EXPECT_FALSE(storage.GetSecret("core", "admin_pw_hash").has_value());
}

TEST_F(StorageTest, CacheRoundTripsAndIsSeparateFromSettings) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "device_list", 1, "settings-value"));
    ASSERT_TRUE(storage.WriteCache("harmony", "device_list", 1, "cache-value"));

    EXPECT_EQ(storage.GetSetting("harmony", "device_list")->value, "settings-value");
    EXPECT_EQ(storage.ReadCache("harmony", "device_list")->value, "cache-value");
}

TEST_F(StorageTest, EraseCacheRemovesIt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.WriteCache("harmony", "device_list", 1, "cache-value"));
    ASSERT_TRUE(storage.EraseCache("harmony", "device_list"));

    EXPECT_FALSE(storage.ReadCache("harmony", "device_list").has_value());
}

TEST_F(StorageTest, ListAllSettingsReturnsEveryEntryAcrossModules) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.SetSetting("core", "device_name", 2, "living-room"));

    auto entries = storage.ListAllSettings();
    ASSERT_EQ(entries.size(), 2u);
    auto find = [&](const std::string& module_id, const std::string& key) -> const homedeck::SettingEntry* {
        for (const auto& entry : entries) {
            if (entry.module_id == module_id && entry.key == key) return &entry;
        }
        return nullptr;
    };
    const homedeck::SettingEntry* hub_ip = find("harmony", "hub_ip");
    ASSERT_NE(hub_ip, nullptr);
    EXPECT_EQ(hub_ip->schema_version, 1);
    EXPECT_EQ(hub_ip->value, "10.0.0.5");
    const homedeck::SettingEntry* device_name = find("core", "device_name");
    ASSERT_NE(device_name, nullptr);
    EXPECT_EQ(device_name->schema_version, 2);
    EXPECT_EQ(device_name->value, "living-room");
}

TEST_F(StorageTest, ListAllSettingsSkipsEntriesThatFailToDecode) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    // Written directly through the underlying store, bypassing Storage's
    // own Encode() - simulates something not written through Storage
    // ending up in NVS, which ListAllSettings must not choke on.
    ASSERT_TRUE(settings_store.Set("harmony", "malformed", "no-schema-version-prefix"));

    auto entries = storage.ListAllSettings();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].key, "hub_ip");
}

// The reserved-key guard (docs/decisions/ADR-0023-settings-backup-api.md)
// - SettingsStore and SecretStore share the same physical NVS namespace
// on firmware, so an unguarded generic settings write/list could reach
// the admin password hash through the wrong door.
TEST_F(StorageTest, SetSettingRejectsTheReservedAdminPasswordKey) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    EXPECT_FALSE(storage.SetSetting(homedeck::AdminAuthService::kModuleId, homedeck::AdminAuthService::kPasswordKey,
                                     1, "attempted-overwrite"));
    EXPECT_FALSE(storage.GetSetting(homedeck::AdminAuthService::kModuleId, homedeck::AdminAuthService::kPasswordKey)
                     .has_value());
}

TEST_F(StorageTest, ListAllSettingsExcludesTheReservedAdminPasswordKeyEvenIfPresent) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    // Written directly through the underlying store - SetSetting's own
    // guard (tested above) would refuse this, but ListAllSettings must
    // independently exclude it too, in case it ever got there some
    // other way.
    ASSERT_TRUE(settings_store.Set(homedeck::AdminAuthService::kModuleId, homedeck::AdminAuthService::kPasswordKey,
                                    "1\nsomehow-present"));
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));

    auto entries = storage.ListAllSettings();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].key, "hub_ip");
}
