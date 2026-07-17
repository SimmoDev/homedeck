#include "core/storage.h"
#include "platform/host/cache_store.h"
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
    homedeck::Storage storage(settings_store, cache_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));

    auto result = storage.GetSetting("harmony", "hub_ip");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->schema_version, 1);
    EXPECT_EQ(result->value, "10.0.0.5");
}

TEST_F(StorageTest, GetSettingOnMissingKeyReturnsNullopt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store);

    EXPECT_FALSE(storage.GetSetting("harmony", "never_written").has_value());
}

TEST_F(StorageTest, SettingsAreNamespacedPerModule) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.SetSetting("kodi", "hub_ip", 1, "10.0.0.9"));

    EXPECT_EQ(storage.GetSetting("harmony", "hub_ip")->value, "10.0.0.5");
    EXPECT_EQ(storage.GetSetting("kodi", "hub_ip")->value, "10.0.0.9");
}

TEST_F(StorageTest, OverwritingASettingUpdatesBothVersionAndValue) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store);

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
    homedeck::Storage storage(settings_store, cache_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_ip", 1, "10.0.0.5"));
    ASSERT_TRUE(storage.EraseSetting("harmony", "hub_ip"));

    EXPECT_FALSE(storage.GetSetting("harmony", "hub_ip").has_value());
}

TEST_F(StorageTest, CacheRoundTripsAndIsSeparateFromSettings) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store);

    ASSERT_TRUE(storage.SetSetting("harmony", "device_list", 1, "settings-value"));
    ASSERT_TRUE(storage.WriteCache("harmony", "device_list", 1, "cache-value"));

    EXPECT_EQ(storage.GetSetting("harmony", "device_list")->value, "settings-value");
    EXPECT_EQ(storage.ReadCache("harmony", "device_list")->value, "cache-value");
}

TEST_F(StorageTest, EraseCacheRemovesIt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store);

    ASSERT_TRUE(storage.WriteCache("harmony", "device_list", 1, "cache-value"));
    ASSERT_TRUE(storage.EraseCache("harmony", "device_list"));

    EXPECT_FALSE(storage.ReadCache("harmony", "device_list").has_value());
}
