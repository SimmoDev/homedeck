#include "core/logger.h"

#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "third_party/nlohmann/json.hpp"

#include <gtest/gtest.h>

#include <filesystem>

namespace {

class FakeTimeSource : public homedeck::TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override { return fixed_time; }

    std::chrono::system_clock::time_point fixed_time = std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
};

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_logger_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    FakeTimeSource time_source_;
};

}  // namespace

TEST_F(LoggerTest, ReadAllReturnsEmptyArrayWhenNothingLogged) {
    homedeck::Logger logger(*storage_, time_source_);

    EXPECT_EQ(logger.ReadAll(), "[]");
}

TEST_F(LoggerTest, LogRoundTrips) {
    homedeck::Logger logger(*storage_, time_source_);

    logger.Log(homedeck::LogLevel::kWarning, "wifi", "Connection dropped");

    auto parsed = nlohmann::json::parse(logger.ReadAll());
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0]["level"], "warning");
    EXPECT_EQ(parsed[0]["component"], "wifi");
    EXPECT_EQ(parsed[0]["message"], "Connection dropped");
    EXPECT_EQ(parsed[0]["timestamp"], 1700000000);
}

TEST_F(LoggerTest, MultipleEntriesAccumulateInOrder) {
    homedeck::Logger logger(*storage_, time_source_);

    logger.Log(homedeck::LogLevel::kInfo, "wifi", "first");
    logger.Log(homedeck::LogLevel::kInfo, "wifi", "second");
    logger.Log(homedeck::LogLevel::kInfo, "wifi", "third");

    auto parsed = nlohmann::json::parse(logger.ReadAll());
    ASSERT_EQ(parsed.size(), 3u);
    EXPECT_EQ(parsed[0]["message"], "first");
    EXPECT_EQ(parsed[1]["message"], "second");
    EXPECT_EQ(parsed[2]["message"], "third");
}

TEST_F(LoggerTest, RotatesOnceSizeThresholdIsExceeded) {
    // A tiny cap - well under one entry's real size - so the second
    // Log() call is guaranteed to push the running total over it and
    // trigger rotation, without needing hundreds of calls to actually
    // cross the real 64KB production default.
    homedeck::Logger logger(*storage_, time_source_, /*max_log_file_bytes=*/10);

    // ReadAll() between calls flushes the background worker, forcing
    // "first" into its own write instead of being batched together with
    // "second" - see ADR-0020: Log() calls that land close together are
    // deliberately coalesced into one write, so without this the two
    // entries below would arrive as a single batch and never trigger a
    // rotation between them.
    logger.Log(homedeck::LogLevel::kInfo, "core", "first");
    logger.ReadAll();
    logger.Log(homedeck::LogLevel::kInfo, "core", "second");

    // Both entries still readable - the first moved to the rotated slot
    // rather than being lost, and ReadAll() returns rotated+current
    // concatenated oldest-first.
    auto parsed = nlohmann::json::parse(logger.ReadAll());
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0]["message"], "first");
    EXPECT_EQ(parsed[1]["message"], "second");

    // The rotated cache key genuinely holds the first entry, not just
    // an artifact of ReadAll()'s own ordering.
    auto rotated = storage_->ReadCache("core", "log_rotated");
    ASSERT_TRUE(rotated.has_value());
    EXPECT_NE(rotated->value.find("first"), std::string::npos);
}

TEST_F(LoggerTest, RotationOverwritesThePreviousRotatedEntry) {
    homedeck::Logger logger(*storage_, time_source_, /*max_log_file_bytes=*/10);

    // Each Log()/ReadAll() pair forces its own batch boundary - see the
    // identical note in RotatesOnceSizeThresholdIsExceeded above.
    logger.Log(homedeck::LogLevel::kInfo, "core", "one");
    logger.ReadAll();
    logger.Log(homedeck::LogLevel::kInfo, "core", "two");
    logger.ReadAll();
    logger.Log(homedeck::LogLevel::kInfo, "core", "three");

    // "one" was rotated out for real once "two" pushed past the cap,
    // then rotated out of existence entirely once "three" pushed past
    // it again - only the two most recent entries survive by design
    // (bounded retention, not unbounded history).
    auto parsed = nlohmann::json::parse(logger.ReadAll());
    ASSERT_EQ(parsed.size(), 2u);
    EXPECT_EQ(parsed[0]["message"], "two");
    EXPECT_EQ(parsed[1]["message"], "three");
}

TEST_F(LoggerTest, ConcurrentLogCallsAreCoalescedIntoOneBatch) {
    // The whole point of ADR-0020's redesign: Log() calls that land
    // close together (no flush between them) are written together, not
    // as separate flash writes - this is what fixed a real display
    // glitch on hardware (see docs/architecture/ui.md#status). The
    // default cap is plenty for three short entries in one batch; the
    // "no rotation happened" assertion below is what actually proves
    // they landed in a single write rather than three.
    homedeck::Logger logger(*storage_, time_source_);

    logger.Log(homedeck::LogLevel::kInfo, "wifi", "a");
    logger.Log(homedeck::LogLevel::kInfo, "mdns", "b");
    logger.Log(homedeck::LogLevel::kInfo, "web_server", "c");

    // No rotation happened - all three fit in one batch under the cap,
    // so nothing was ever evicted to the rotated slot.
    auto parsed = nlohmann::json::parse(logger.ReadAll());
    ASSERT_EQ(parsed.size(), 3u);
    EXPECT_EQ(parsed[0]["message"], "a");
    EXPECT_EQ(parsed[1]["message"], "b");
    EXPECT_EQ(parsed[2]["message"], "c");
    EXPECT_FALSE(storage_->ReadCache("core", "log_rotated").has_value());
}
