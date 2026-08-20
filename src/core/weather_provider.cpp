#include "core/weather_provider.h"

#include "core/json_request.h"
#include "third_party/nlohmann/json.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace homedeck {

namespace {

constexpr char kCacheKey[] = "last_reading";
constexpr int kCacheSchemaVersion = 1;

// A full, successful strtod parse of the whole string - not just a
// leading numeric prefix - the same defensive-input-validation
// reasoning CLAUDE.md asks for at a boundary (these values come from
// the Web UI's Settings API, effectively user input). Returns the
// parsed value so IsValidWeatherCoordinate() below can range-check it
// too, instead of parsing the string twice.
std::optional<double> ParseNumber(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return std::nullopt;
    return value;
}

bool ParsesAsNumber(const std::string& text) { return ParseNumber(text).has_value(); }

}  // namespace

bool IsValidWeatherCoordinate(const std::string& key, const std::string& value) {
    double max_abs;
    if (key == OpenMeteoWeatherProvider::kLatitudeKey) {
        max_abs = 90.0;
    } else if (key == OpenMeteoWeatherProvider::kLongitudeKey) {
        max_abs = 180.0;
    } else {
        return true;  // only latitude/longitude have a format worth constraining
    }
    if (value.empty()) return true;
    std::optional<double> parsed = ParseNumber(value);
    return parsed.has_value() && *parsed >= -max_abs && *parsed <= max_abs;
}

OpenMeteoWeatherProvider::OpenMeteoWeatherProvider(HttpClient& http_client, Storage& storage, EventBus& event_bus,
                                                     std::chrono::milliseconds poll_interval)
    : http_client_(http_client),
      storage_(storage),
      event_bus_(event_bus),
      poll_interval_(poll_interval),
      poll_task_("weather-poll", [this](std::stop_token stop) { PollLoop(stop); }) {}

WeatherState OpenMeteoWeatherProvider::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void OpenMeteoWeatherProvider::PollLoop(std::stop_token stop) {
    // Woken on request_stop() too (via this stop_callback), not just by
    // TriggerPoll() or the timeout below - preserves Task::~Task()'s
    // "stops and joins promptly" contract without needing a chunked-
    // sleep workaround.
    std::stop_callback wake_on_stop(stop, [this] { wake_cv_.notify_one(); });

    while (!stop.stop_requested()) {
        PollOnce();

        std::unique_lock<std::mutex> lock(wake_mutex_);
        wake_cv_.wait_for(lock, poll_interval_, [this, &stop] { return wake_requested_ || stop.stop_requested(); });
        wake_requested_ = false;
    }
}

void OpenMeteoWeatherProvider::TriggerPoll() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_requested_ = true;
    }
    wake_cv_.notify_one();
}

void OpenMeteoWeatherProvider::PollOnce() {
    auto latitude = storage_.GetSetting(kModuleId, kLatitudeKey);
    auto longitude = storage_.GetSetting(kModuleId, kLongitudeKey);
    auto display_name = storage_.GetSetting(kModuleId, kDisplayNameKey);

    bool configured = latitude.has_value() && longitude.has_value() && ParsesAsNumber(latitude->value) &&
                       ParsesAsNumber(longitude->value);
    if (!configured) {
        // Published after mutex_ releases (a nested scope, not held across
        // the call) - a synchronous (non-UI Subscribe, not SubscribeUi)
        // WeatherUpdatedEvent subscriber that calls back into Snapshot()
        // from inside its callback would otherwise self-deadlock on this
        // same, non-recursive mutex_. Same reasoning as
        // EventBus::PublishImpl's own comment about copying its subscriber
        // list out from under its lock before invoking callbacks.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = WeatherState{false, false, false, 0.0, 0, ""};
        }
        event_bus_.Publish(WeatherUpdatedEvent{});
        return;
    }

    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + latitude->value +
                       "&longitude=" + longitude->value + "&current=temperature_2m,weather_code";
    HttpClientResponse response = http_client_.Get(url);

    std::string name = display_name.has_value() ? display_name->value : "";

    if (response.success && response.status_code == 200) {
        std::optional<nlohmann::json> parsed = TryParseJsonObject(response.body);
        auto current = parsed.has_value() ? parsed->find("current") : nlohmann::json::iterator{};
        if (parsed.has_value() && current != parsed->end() && current->is_object() &&
            current->contains("temperature_2m") && current->contains("weather_code")) {
            double temperature_c = current->at("temperature_2m").get<double>();
            int weather_code = current->at("weather_code").get<int>();

            nlohmann::json cache = {
                {"temperature_c", temperature_c},
                {"weather_code", weather_code},
                {"display_name", name},
            };
            // A failed write here only risks a reboot before the next
            // successful poll falling back to stale/absent cached data
            // (self-heals on the next successful poll) - but silently,
            // with no trace of why, unless this is reported now.
            if (!storage_.WriteCache(kModuleId, kCacheKey, kCacheSchemaVersion, cache.dump())) {
                std::cerr << "OpenMeteoWeatherProvider: failed to persist weather cache\n";
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = WeatherState{true, true, true, temperature_c, weather_code, name};
            }
            event_bus_.Publish(WeatherUpdatedEvent{});
            return;
        }
    }

    // Fetch failed or returned something unparseable - fall back to the
    // last cached reading (restart-persisted or from an earlier
    // successful poll this session) rather than showing blank, marked
    // non-live so the UI can distinguish it from a fresh reading.
    auto cached = storage_.ReadCache(kModuleId, kCacheKey);
    if (cached.has_value()) {
        std::optional<nlohmann::json> parsed = TryParseJsonObject(cached->value);
        if (parsed.has_value() && parsed->contains("temperature_c") && parsed->contains("weather_code")) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_ = WeatherState{true, true, false, parsed->at("temperature_c").get<double>(),
                                       parsed->at("weather_code").get<int>(), name};
            }
            event_bus_.Publish(WeatherUpdatedEvent{});
            return;
        }
    }
    // Configured, but no reading has ever been obtained (first poll
    // still in flight or failed, and nothing cached from a prior
    // session either) - has_reading=false, not a real 0.0/0 reading.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = WeatherState{true, false, false, 0.0, 0, name};
    }
    event_bus_.Publish(WeatherUpdatedEvent{});
}

}  // namespace homedeck
