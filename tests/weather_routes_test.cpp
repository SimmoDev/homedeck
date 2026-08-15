#include "core/weather_routes.h"

#include "core/admin_auth_service.h"
#include "core/weather_provider.h"
#include "platform/host/cache_store.h"
#include "platform/host/http_server.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"

#include "http_test_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using homedeck::testing::HttpRequestRaw;
using homedeck::testing::HttpResult;
using homedeck::testing::SessionCookieOnly;

// A scriptable HttpClient double - same role as
// weather_provider_test.cpp's own FakeHttpClient, duplicated here
// rather than shared, matching settings_routes_test.cpp's own
// HttpRequestRaw precedent for this exact tradeoff.
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


class WeatherRoutesTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_weather_routes_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
        auth_ = std::make_unique<homedeck::AdminAuthService>(*storage_, time_source_);
        event_bus_ = std::make_unique<homedeck::EventBus>();
        weather_provider_ = std::make_unique<homedeck::OpenMeteoWeatherProvider>(
            geocode_http_client_, *storage_, *event_bus_, std::chrono::hours(1));
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    // Registers auth routes and returns the session cookie from a real
    // setup call - must run after server.Start(), per HttpServer's own
    // contract.
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
    std::unique_ptr<homedeck::EventBus> event_bus_;
    FakeHttpClient geocode_http_client_;
    // Constructed with a far-future poll interval in SetUp() - PollOnce()
    // firing on its own during a test would spuriously inflate
    // geocode_http_client_'s GetCount(), the same reasoning
    // weather_provider_test.cpp's TriggerPollWakesTheLoopImmediately
    // test already documents.
    std::unique_ptr<homedeck::OpenMeteoWeatherProvider> weather_provider_;
};

}  // namespace

TEST_F(WeatherRoutesTest, BothEndpointsRequireAuthentication) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18310));

    EXPECT_EQ(HttpRequestRaw(18310, "GET", "/api/weather/geocode?query=Berlin", "").status_code, 403);
    EXPECT_EQ(HttpRequestRaw(18310, "POST", "/api/weather/refresh", "").status_code, 403);

    auto setup = HttpRequestRaw(18310, "POST", "/api/auth/setup", R"({"password":"correct horse battery"})");
    ASSERT_EQ(setup.status_code, 200);

    EXPECT_EQ(HttpRequestRaw(18310, "GET", "/api/weather/geocode?query=Berlin", "").status_code, 401);
    EXPECT_EQ(HttpRequestRaw(18310, "POST", "/api/weather/refresh", "").status_code, 401);
}

TEST_F(WeatherRoutesTest, GeocodeRejectsMissingQueryParam) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18311));
    std::string cookie = Login(18311);

    auto result = HttpRequestRaw(18311, "GET", "/api/weather/geocode", "", cookie);
    EXPECT_EQ(result.status_code, 400);
    EXPECT_NE(result.body.find(R"("field":"query")"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeRejectsAnOversizedQuery) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18319));
    std::string cookie = Login(18319);

    std::string oversized_query(101, 'a');
    auto result = HttpRequestRaw(18319, "GET", "/api/weather/geocode?query=" + oversized_query, "", cookie);
    EXPECT_EQ(result.status_code, 400);
    EXPECT_NE(result.body.find("query_too_long"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeParsesUpstreamResultsAndSkipsIncompleteEntries) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{
        true, 200,
        R"({"results":[)"
        R"({"name":"Berlin","admin1":"Berlin","country":"Germany","latitude":52.52,"longitude":13.41},)"
        // Missing "longitude" - must be skipped, not crash the handler.
        R"({"name":"Incomplete","latitude":1.0})"
        R"(]})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18312));
    std::string cookie = Login(18312);

    auto result = HttpRequestRaw(18312, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("name":"Berlin")"), std::string::npos);
    EXPECT_EQ(result.body.find("Incomplete"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeSkipsEntriesWithNonNumericLatitudeOrLongitude) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{
        true, 200,
        R"({"results":[)"
        R"({"name":"Berlin","admin1":"Berlin","country":"Germany","latitude":52.52,"longitude":13.41},)"
        // Present but wrong-typed - must be skipped, not abort the
        // process (nlohmann's .get<double>() on a non-numeric value
        // calls std::abort() since exceptions are disabled on the
        // firmware target).
        R"({"name":"WrongType","latitude":"not-a-number","longitude":13.41})"
        R"(]})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18320));
    std::string cookie = Login(18320);

    auto result = HttpRequestRaw(18320, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("name":"Berlin")"), std::string::npos);
    EXPECT_EQ(result.body.find("WrongType"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeSkipsEntriesWithNonStringName) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{
        true, 200,
        R"({"results":[)"
        R"({"name":"Berlin","admin1":"Berlin","country":"Germany","latitude":52.52,"longitude":13.41},)"
        // "name" present but wrong-typed - must be skipped, not abort
        // the process (nlohmann's .get<std::string>() on a non-string
        // value calls std::abort() since exceptions are disabled on the
        // firmware target).
        R"({"name":404,"latitude":1.0,"longitude":2.0})"
        R"(]})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18321));
    std::string cookie = Login(18321);

    auto result = HttpRequestRaw(18321, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("name":"Berlin")"), std::string::npos);
    EXPECT_EQ(result.body.find(R"("latitude":1.0)"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeDefaultsAdmin1AndCountryToEmptyWhenWrongTyped) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{
        true, 200,
        R"({"results":[)"
        // admin1/country present but wrong-typed - unlike name/latitude/
        // longitude, this shouldn't drop the entry, just default those
        // two fields to empty instead of aborting the process.
        R"({"name":"Berlin","admin1":123,"country":null,"latitude":52.52,"longitude":13.41})"
        R"(]})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18322));
    std::string cookie = Login(18322);

    auto result = HttpRequestRaw(18322, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.body.find(R"("name":"Berlin")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("admin1":"")"), std::string::npos);
    EXPECT_NE(result.body.find(R"("country":"")"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeUrlEncodesTheQueryBeforeForwarding) {
    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18313));
    std::string cookie = Login(18313);

    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{true, 200, R"({"results":[]})"});
    // "New York" contains a space - must be percent-encoded (or at
    // least not left as a literal space) on the outbound Open-Meteo
    // URL, per UrlEncode's own comment on why an unescaped space would
    // break the request. This test only proves the handler completes
    // successfully with a query containing reserved characters; the
    // FakeHttpClient doesn't capture the outbound URL to inspect
    // directly.
    auto result = HttpRequestRaw(18313, "GET", "/api/weather/geocode?query=New%20York", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(geocode_http_client_.GetCount(), 1);
}

TEST_F(WeatherRoutesTest, GeocodeReturns502WhenUpstreamRequestFails) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{false, 0, ""});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18314));
    std::string cookie = Login(18314);

    auto result = HttpRequestRaw(18314, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 502);
    EXPECT_NE(result.body.find("upstream_failed"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeReturns502OnNon200UpstreamStatus) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{true, 500, "Internal Server Error"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18315));
    std::string cookie = Login(18315);

    auto result = HttpRequestRaw(18315, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 502);
    EXPECT_NE(result.body.find("upstream_failed"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeReturns502OnMalformedUpstreamJson) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{true, 200, "not json"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18316));
    std::string cookie = Login(18316);

    auto result = HttpRequestRaw(18316, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 502);
    EXPECT_NE(result.body.find("upstream_invalid_response"), std::string::npos);
}

TEST_F(WeatherRoutesTest, GeocodeReturnsEmptyResultsWhenUpstreamOmitsResultsKey) {
    geocode_http_client_.SetResponse(homedeck::HttpClientResponse{true, 200, R"({"generationtime_ms":0.1})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18317));
    std::string cookie = Login(18317);

    auto result = HttpRequestRaw(18317, "GET", "/api/weather/geocode?query=Berlin", "", cookie);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, R"({"results":[]})");
}

TEST_F(WeatherRoutesTest, RefreshTriggersAnImmediatePoll) {
    // Configured (unlike the other tests here) so TriggerPoll()'s own
    // PollOnce() actually reaches Get() - proving the endpoint is wired
    // to the real provider, not just returning 200 without effect.
    ASSERT_TRUE(storage_->SetSetting("weather", "latitude", 1, "52.52"));
    ASSERT_TRUE(storage_->SetSetting("weather", "longitude", 1, "13.41"));
    geocode_http_client_.SetResponse(
        homedeck::HttpClientResponse{true, 200, R"({"current":{"temperature_2m":13.7,"weather_code":0}})"});

    homedeck::HostHttpServer server;
    homedeck::RegisterAdminAuthRoutes(server, *auth_);
    homedeck::RegisterWeatherRoutes(server, geocode_http_client_, *weather_provider_, *auth_);
    ASSERT_TRUE(server.Start(18318));
    std::string cookie = Login(18318);

    int count_before = geocode_http_client_.GetCount();
    auto result = HttpRequestRaw(18318, "POST", "/api/weather/refresh", "", cookie);
    EXPECT_EQ(result.status_code, 200);

    for (int i = 0; i < 200 && geocode_http_client_.GetCount() == count_before; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GT(geocode_http_client_.GetCount(), count_before);
}
