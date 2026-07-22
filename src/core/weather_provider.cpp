#include "core/weather_provider.h"

#include "third_party/nlohmann/json.hpp"

#include <chrono>
#include <cstdlib>

namespace homedeck {

namespace {

constexpr char kModuleId[] = "weather";
constexpr char kLatitudeKey[] = "latitude";
constexpr char kLongitudeKey[] = "longitude";
constexpr char kDisplayNameKey[] = "display_name";
constexpr char kCacheKey[] = "last_reading";
constexpr int kCacheSchemaVersion = 1;

// A full, successful strtod parse of the whole string - not just a
// leading numeric prefix - the same defensive-input-validation
// reasoning CLAUDE.md asks for at a boundary (these values come from
// the Web UI's Settings API, effectively user input).
bool ParsesAsNumber(const std::string& text) {
    if (text.empty()) return false;
    char* end = nullptr;
    std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size();
}

}  // namespace

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
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = WeatherState{false, false, false, 0.0, 0, ""};
        event_bus_.Publish(WeatherUpdatedEvent{});
        return;
    }

    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + latitude->value +
                       "&longitude=" + longitude->value + "&current=temperature_2m,weather_code";
    HttpClientResponse response = http_client_.Get(url);

    std::string name = display_name.has_value() ? display_name->value : "";

    if (response.success && response.status_code == 200) {
        nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
        auto current = parsed.is_object() ? parsed.find("current") : parsed.end();
        if (!parsed.is_discarded() && current != parsed.end() && current->is_object() &&
            current->contains("temperature_2m") && current->contains("weather_code")) {
            double temperature_c = current->at("temperature_2m").get<double>();
            int weather_code = current->at("weather_code").get<int>();

            nlohmann::json cache = {
                {"temperature_c", temperature_c},
                {"weather_code", weather_code},
                {"display_name", name},
            };
            storage_.WriteCache(kModuleId, kCacheKey, kCacheSchemaVersion, cache.dump());

            std::lock_guard<std::mutex> lock(mutex_);
            state_ = WeatherState{true, true, true, temperature_c, weather_code, name};
            event_bus_.Publish(WeatherUpdatedEvent{});
            return;
        }
    }

    // Fetch failed or returned something unparseable - fall back to the
    // last cached reading (restart-persisted or from an earlier
    // successful poll this session) rather than showing blank, marked
    // non-live so the UI can distinguish it from a fresh reading.
    auto cached = storage_.ReadCache(kModuleId, kCacheKey);
    std::lock_guard<std::mutex> lock(mutex_);
    if (cached.has_value()) {
        nlohmann::json parsed = nlohmann::json::parse(cached->value, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("temperature_c") &&
            parsed.contains("weather_code")) {
            state_ = WeatherState{true, true, false, parsed.at("temperature_c").get<double>(),
                                   parsed.at("weather_code").get<int>(), name};
            event_bus_.Publish(WeatherUpdatedEvent{});
            return;
        }
    }
    // Configured, but no reading has ever been obtained (first poll
    // still in flight or failed, and nothing cached from a prior
    // session either) - has_reading=false, not a real 0.0/0 reading.
    state_ = WeatherState{true, false, false, 0.0, 0, name};
    event_bus_.Publish(WeatherUpdatedEvent{});
}

}  // namespace homedeck
