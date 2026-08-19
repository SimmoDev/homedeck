#include "core/weather_provider.h"

#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {

// A scriptable HttpClient double - controls exact response content
// (unlike HostHttpClient's real network I/O) so tests can deterministically
// exercise success/failure/malformed-body paths without depending on a
// real server or the live internet.
class FakeHttpClient : public homedeck::HttpClient {
public:
    homedeck::HttpClientResponse Get(const std::string& /*url*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        get_count_++;
        return response_;
    }

    // Not exercised by this test file's own weather-focused cases - only
    // present so this fake satisfies HttpClient's full interface.
    homedeck::HttpClientResponse Post(const std::string& /*url*/, const std::string& /*json_body*/,
                                       const std::vector<std::pair<std::string, std::string>>& /*extra_headers*/ = {}) override {
        return response_;
    }

    void SetResponse(homedeck::HttpClientResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
    }

    int GetCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_count_;
    }

private:
    std::mutex mutex_;
    homedeck::HttpClientResponse response_{false, 0, ""};
    int get_count_ = 0;
};

// Polls Snapshot() from the test thread until the predicate holds or a
// bounded number of short sleeps elapse - the poll Task runs on its own
// thread, so tests can't just assert immediately after construction/
// SetResponse(). Same capped sleep-loop pattern task_test.cpp itself
// uses.
template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 200) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class WeatherProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_weather_provider_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
};

constexpr std::chrono::milliseconds kFastPollInterval = std::chrono::milliseconds(30);

const char kSuccessBody[] = R"({"current":{"temperature_2m":13.7,"weather_code":0}})";

}  // namespace

TEST_F(WeatherProviderTest, NotConfiguredReportsConfiguredFalse) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::EventBus bus;
    FakeHttpClient http_client;

    std::atomic<int> events{0};
    auto sub = bus.Subscribe<homedeck::WeatherUpdatedEvent>(
        [&events](const homedeck::WeatherUpdatedEvent&) { events++; });

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    // PollOnce() publishes WeatherUpdatedEvent every cycle regardless of
    // configured state - GetCount() can't be the readiness signal here
    // since the not-configured path never calls Get() at all.
    ASSERT_TRUE(WaitFor([&] { return events.load() > 0; }));

    homedeck::WeatherState state = provider.Snapshot();
    EXPECT_FALSE(state.configured);
    EXPECT_FALSE(state.has_reading);
    EXPECT_EQ(http_client.GetCount(), 0);  // never attempted a fetch - nothing configured
}

TEST_F(WeatherProviderTest, ConfiguredAndSuccessfulFetchReportsLiveReading) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));
    ASSERT_TRUE(storage.SetSetting("weather", "display_name", 1, "Berlin, DE"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kSuccessBody});

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    ASSERT_TRUE(WaitFor([&] { return provider.Snapshot().has_reading; }));

    homedeck::WeatherState state = provider.Snapshot();
    EXPECT_TRUE(state.configured);
    EXPECT_TRUE(state.has_reading);
    EXPECT_TRUE(state.live);
    EXPECT_DOUBLE_EQ(state.temperature_c, 13.7);
    EXPECT_EQ(state.weather_code, 0);
    EXPECT_EQ(state.display_name, "Berlin, DE");

    // Persisted for restart-restore - see storage.h's ReadCache.
    auto cached = storage.ReadCache("weather", "last_reading");
    ASSERT_TRUE(cached.has_value());
}

TEST_F(WeatherProviderTest, FetchFailureWithNoPriorCacheReportsNoReading) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    ASSERT_TRUE(WaitFor([&] { return http_client.GetCount() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    homedeck::WeatherState state = provider.Snapshot();
    EXPECT_TRUE(state.configured);
    EXPECT_FALSE(state.has_reading);
    EXPECT_FALSE(state.live);
}

TEST_F(WeatherProviderTest, ExcessivelyNestedForecastResponseReportsNoReading) {
    // The same stack-overflow-guard the harmony_connection_test.cpp
    // handshake/config-fetch tests exercise, here for the forecast
    // fetch's own parse - both must go through the same bounded parser
    // (TryParseJsonObject()) since both parse a network-supplied body.
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    std::string nested_body;
    for (int i = 0; i < 40; ++i) nested_body += "[";
    for (int i = 0; i < 40; ++i) nested_body += "]";
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, nested_body});

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    ASSERT_TRUE(WaitFor([&] { return http_client.GetCount() > 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    homedeck::WeatherState state = provider.Snapshot();
    EXPECT_TRUE(state.configured);
    EXPECT_FALSE(state.has_reading);
    EXPECT_FALSE(state.live);
}

TEST_F(WeatherProviderTest, FetchFailureFallsBackToPriorCache) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));
    ASSERT_TRUE(storage.SetSetting("weather", "display_name", 1, "Berlin, DE"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kSuccessBody});

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    // Let a real successful fetch land and get cached first.
    ASSERT_TRUE(WaitFor([&] { return provider.Snapshot().live; }));

    // Now start failing.
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});
    ASSERT_TRUE(WaitFor([&] { return provider.Snapshot().has_reading && !provider.Snapshot().live; }));

    homedeck::WeatherState state = provider.Snapshot();
    EXPECT_TRUE(state.configured);
    EXPECT_TRUE(state.has_reading);
    EXPECT_FALSE(state.live);
    EXPECT_DOUBLE_EQ(state.temperature_c, 13.7);
    EXPECT_EQ(state.weather_code, 0);
}

TEST_F(WeatherProviderTest, PollsMoreThanOnceOverTime) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kSuccessBody});

    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, kFastPollInterval);

    ASSERT_TRUE(WaitFor([&] { return http_client.GetCount() >= 3; }, /*max_attempts=*/500));
}

TEST_F(WeatherProviderTest, TriggerPollWakesTheLoopImmediately) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);

    homedeck::EventBus bus;
    FakeHttpClient http_client;

    // A poll interval far longer than this test's own timeout - the
    // point is proving TriggerPoll() doesn't wait for it, the same
    // "not configured" trick the other tests use to avoid depending on
    // a real fetch (see NotConfiguredReportsConfiguredFalse).
    homedeck::OpenMeteoWeatherProvider provider(http_client, storage, bus, std::chrono::hours(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the first (not-configured) PollOnce() land

    ASSERT_TRUE(storage.SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage.SetSetting("weather", "longitude", 1, "13.41"));
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kSuccessBody});

    provider.TriggerPoll();

    ASSERT_TRUE(WaitFor([&] { return provider.Snapshot().live; }));
}
