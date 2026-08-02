#pragma once

#include "core/admin_auth_service.h"
#include "core/clock.h"
#include "core/critical_battery_monitor.h"
#include "core/diagnostics_routes.h"
#include "core/event_bus.h"
#include "core/logger.h"
#include "core/low_battery_monitor.h"
#include "core/network_status_monitor.h"
#include "core/notification_sound.h"
#include "core/ota_routes.h"
#include "core/power_manager.h"
#include "core/settings_routes.h"
#include "core/storage.h"
#include "core/weather_provider.h"
#include "core/weather_routes.h"
#include "core/wifi_routes.h"
#include "platform/audio_output.h"
#include "platform/battery_reader.h"
#include "platform/cache_store.h"
#include "platform/display_brightness.h"
#include "platform/http_client.h"
#include "platform/http_server.h"
#include "platform/network_status.h"
#include "platform/secret_store.h"
#include "platform/settings_store.h"
#include "platform/steady_time_source.h"
#include "platform/time_source.h"
#include "platform/user_activity_source.h"
#include "ui/clock_widget.h"
#include "ui/navigation.h"
#include "ui/network_status_widget.h"
#include "ui/notification_banner.h"
#include "ui/notification_widget.h"
#include "ui/quick_settings_panel.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/screens/wifi_setup_screen.h"
#include "ui/weather_widget.h"

namespace homedeck {

// The shared object graph both firmware/main/homedeck.cpp and
// simulator/main.cpp build against - every Core service and dashboard
// widget from Storage down through the Web Management UI's route
// registration. Each entry point still owns what's genuinely
// platform-specific: constructing the concrete platform backends passed
// in below, Wi-Fi/mDNS bring-up (firmware) or debug buttons (simulator),
// and the run loop.
//
// Two-phase on purpose: the constructor builds every member below with
// no ordering requirement on the caller, then Start() (see its own
// comment) publishes Clock's first tick once every subscriber
// constructed above is guaranteed to already exist - this class is what
// enforces that ordering, rather than leaving it to convention across
// each entry point's own sequential construction.
class AppCore {
public:
    // Everything genuinely platform-specific that AppCore still needs
    // injected - the concrete backends behind Core's small abstract
    // interfaces (see src/README.md), plus the handful of callbacks each
    // target answers differently. See SetOnDeviceNameChanged() below for
    // the one callback that isn't here.
    struct Dependencies {
        BatteryReader& battery_reader;
        NetworkStatus& network_status;
        AudioOutput& audio_output;
        DisplayBrightness& display_brightness;
        UserActivitySource& user_activity_source;
        SettingsStore& settings_store;
        CacheStore& cache_store;
        SecretStore& secret_store;
        HttpClient& http_client;
        HttpServer& http_server;
        // Wall-clock time - Clock and Logger's shared source. Firmware
        // passes Rx8130TimeSource, the simulator HostTimeSource.
        TimeSource& time_source;
        WifiSetupScreen::SubmitCallback wifi_submit;
        WifiResetFn wifi_reset;
        OtaWriter ota_writer;
        OtaRebootFn ota_reboot;
        CoreDumpReader read_core_dump;
    };

    // Builds the full object graph and registers every Web UI route
    // against http_server (still requires the caller to call
    // http_server.Start() afterward - static asset serving and the
    // server's own listen call stay outside, since embedding source and
    // port both differ per target). Must run inside whatever LVGL lock
    // discipline the caller already holds (see ADR-0011) - this
    // constructs LVGL objects directly (DashboardScreen and its widgets,
    // WifiSetupScreen, QuickSettingsPanel), the same requirement every
    // other direct LVGL call in the former app_main()/main() already had.
    AppCore(EventBus& event_bus, Dependencies deps);

    // Publishes Clock's first tick - call once, after construction,
    // before Wi-Fi/mDNS bring-up or the run loop starts. See clock.h's
    // own comment for why this is a separate call rather than happening
    // automatically.
    void Start();

    // Optional; invoked when the device name setting changes via the
    // Web UI's Settings page (see settings_routes.h's
    // DeviceNameChangedFn) - e.g. firmware's mDNS re-announce, which
    // needs GetLogger() below. Deliberately settable after construction,
    // not a Dependencies member: RegisterSettingsRoutes is called from
    // AppCore's own constructor, before a caller-supplied callback could
    // reference GetLogger() safely. No-op (name change always accepted)
    // if never set - the simulator's case, which has no mDNS to
    // re-announce.
    void SetOnDeviceNameChanged(DeviceNameChangedFn fn) { on_device_name_changed_ = std::move(fn); }

    Storage& GetStorage() { return storage_; }
    DashboardScreen& GetDashboard() { return dashboard_; }
    Navigation& GetNavigation() { return navigation_; }
    WifiSetupScreen& GetWifiSetupScreen() { return wifi_setup_screen_; }
    Logger& GetLogger() { return logger_; }

private:
    Storage storage_;
    HttpClient& http_client_;
    OpenMeteoWeatherProvider weather_provider_;

    DashboardScreen dashboard_;
    ClockWidget clock_widget_;
    NetworkStatusWidget network_status_widget_;
    WeatherWidget weather_widget_;
    NotificationWidget notification_widget_;

    NotificationBanner notification_banner_;
    NotificationSound notification_sound_;
    LowBatteryMonitor low_battery_monitor_;
    CriticalBatteryMonitor critical_battery_monitor_;
    NetworkStatusMonitor network_status_monitor_;

    SteadyTimeSource power_time_source_;
    PowerManager power_manager_;
    QuickSettingsPanel quick_settings_panel_;

    Navigation navigation_;
    WifiSetupScreen wifi_setup_screen_;

    Clock clock_;
    Logger logger_;

    SteadyTimeSource auth_time_source_;
    AdminAuthService admin_auth_;

    DeviceNameChangedFn on_device_name_changed_;
};

}  // namespace homedeck
