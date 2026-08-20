#pragma once

#include "core/event_bus.h"
#include "core/storage.h"
#include "platform/http_client.h"
#include "platform/task.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace homedeck {

struct WeatherState {
    bool configured;      // false until latitude/longitude are set
    // False in the brief window after configuring but before any
    // fetch has ever succeeded (first poll still in flight, or failed
    // with no prior cache) - without this, temperature_c/weather_code
    // would default to 0.0/0, which happens to also be a real WMO
    // reading ("clear sky, 0C"), not an obvious "no data yet" sentinel.
    // Only temperature_c/weather_code/live are meaningful once true.
    bool has_reading;
    bool live;             // true if this reflects the most recent
                           // successful fetch; false if it's a stale
                           // cached fallback (fetch failed/offline) or
                           // a restart-restored reading
    double temperature_c;
    int weather_code;      // WMO code from Open-Meteo - see
                           // weather_widget.cpp for the text mapping
    std::string display_name;  // e.g. "Berlin, DE" - from Storage, not
                                // part of the weather API response
};

struct WeatherUpdatedEvent {};  // marker only - handlers call Snapshot()

// Empty (unset) or a value that parses fully as a number within the
// valid range for `key` (OpenMeteoWeatherProvider::kLatitudeKey:
// [-90, 90]; kLongitudeKey: [-180, 180]; any other key: always true,
// since only these two keys have a format worth constraining) - the
// same numeric-format check Snapshot()'s own "configured" gate already
// applies, plus the range bound that gate doesn't need but a stored
// value should never violate. Mirrors IsValidHubHost()'s role: invoked
// from POST /api/settings via core/settings_routes.h's generic
// SettingValidateFn (wired in ui/app_core.cpp), since the generic
// settings API - not a dedicated weather-config endpoint - is what
// actually persists these values. display_name carries no format
// constraint beyond the settings API's own generic length cap - it's
// just a label.
bool IsValidWeatherCoordinate(const std::string& key, const std::string& value);

// See docs/architecture/networking.md and ADR-0008's "Weather data
// source" decision. No EventBus dependency here by design, same
// reasoning as NetworkStatus (core/network_status_monitor.h) - except
// unlike NetworkStatus, this interface's implementation genuinely does
// need EventBus itself, since fetching weather is inherently async
// (a network call, not a fast hardware-state read) - the interface
// only commits callers to a synchronous Snapshot(), not to how a given
// implementation gets there.
class WeatherProvider {
public:
    virtual ~WeatherProvider() = default;

    virtual WeatherState Snapshot() const = 0;
};

// The direct provider ADR-0008 decided on - Open-Meteo, no API key.
// Portable (not platform/firmware vs platform/host split): built
// entirely on already-portable interfaces (HttpClient&, Storage&,
// EventBus&), so one implementation serves both targets, the same
// shape Storage itself already has.
class OpenMeteoWeatherProvider : public WeatherProvider {
public:
    // Module/key names for the generic /api/settings API - shared
    // between this class's own Storage reads and ui/app_core.cpp's
    // SettingValidateFn wiring, same as HarmonyConnection::kModuleId/
    // kHubHostKey.
    static constexpr char kModuleId[] = "weather";
    static constexpr char kLatitudeKey[] = "latitude";
    static constexpr char kLongitudeKey[] = "longitude";
    static constexpr char kDisplayNameKey[] = "display_name";

    // poll_interval is injectable (defaulted to a real 30 minutes) so
    // tests can use a millisecond-scale interval instead of waiting -
    // the same "real default, test-overridable" shape Clock::tick_period
    // already uses.
    OpenMeteoWeatherProvider(HttpClient& http_client, Storage& storage, EventBus& event_bus,
                              std::chrono::milliseconds poll_interval = std::chrono::minutes(30));

    WeatherState Snapshot() const override;

    // Wakes the poll loop immediately rather than waiting out the rest
    // of poll_interval_ - the Web UI's weather settings save flow calls
    // this (see core/weather_routes.cpp's POST /api/weather/refresh) so
    // a newly-chosen location shows up promptly instead of after up to
    // 30 real minutes of silence. Asynchronous: returns immediately,
    // the actual fetch happens on poll_task_'s own thread as usual, not
    // on the caller's.
    void TriggerPoll();

private:
    void PollOnce();
    void PollLoop(std::stop_token stop);

    HttpClient& http_client_;
    Storage& storage_;
    EventBus& event_bus_;
    std::chrono::milliseconds poll_interval_;

    // Written only by poll_task_'s own thread, read from the UI thread
    // via Snapshot() - same single-writer/multi-reader mutex pattern as
    // FirmwareNetworkStatus.
    mutable std::mutex mutex_;
    WeatherState state_{false, false, false, 0.0, 0, ""};

    // Guards wake_requested_ below - separate from mutex_ above (which
    // guards state_) since TriggerPoll() and Snapshot() are called from
    // different, unrelated contexts and have no reason to contend with
    // each other.
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_requested_ = false;

    // Not a Timer: platform/firmware/timer.cpp's own comment confirms
    // all FreeRTOS software timers share one timer-service task, sized
    // for lightweight callbacks like Clock's tick - a multi-second
    // blocking HTTPS fetch inside that shared callback would stall
    // every other timer in the system, including Clock. A dedicated
    // Task avoids that entirely. Declared last (not just initialized
    // last) - members are constructed in declaration order, and
    // poll_task_'s thread starts running PollLoop() immediately, which
    // touches every member above it, so all of them must already be
    // fully constructed first.
    Task poll_task_;
};

}  // namespace homedeck
