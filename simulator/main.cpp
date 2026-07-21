#include "core/admin_auth_service.h"
#include "core/clock.h"
#include "core/diagnostics_routes.h"
#include "core/event_bus.h"
#include "core/logger.h"
#include "core/low_battery_monitor.h"
#include "core/network_status_monitor.h"
#include "core/ota_routes.h"
#include "core/settings_routes.h"
#include "platform/host/battery_reader.h"
#include "platform/host/cache_store.h"
#include "platform/host/file_backed_store.h"
#include "platform/host/http_server.h"
#include "platform/host/network_status.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"
#include "platform/static_assets.h"
#include "platform/steady_time_source.h"
#include "ui/clock_widget.h"
#include "ui/navigation.h"
#include "ui/notification_banner.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/screens/wifi_setup_screen.h"
#include "ui/ui_task.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <tuple>
#include <vector>

// Matches the Tab5's native panel resolution and orientation - portrait,
// not the 1280x720 spec-sheet landscape figure (see
// docs/architecture/hardware.md#display-driver-strategy for why).
static constexpr int32_t kWindowWidth = 720;
static constexpr int32_t kWindowHeight = 1280;

// Desktop dev-convenience only: scales the on-screen window so a
// 720x1280 logical canvas doesn't demand 1280px of vertical monitor
// space. LVGL still renders at the real logical resolution above -
// layout behaves identically to hardware, just displayed smaller. Any
// zoom other than 1.0 softens text somewhat, even with UiTask's
// scale-quality hint (see src/ui/ui_task.cpp) - real hardware is
// unaffected, since it never scales at all. No single value fits every
// desktop/taskbar layout, so override at runtime with HOMEDECK_SIM_ZOOM
// (e.g. `HOMEDECK_SIM_ZOOM=0.6 ./homedeck_simulator`) rather than editing
// this default.
static constexpr float kDefaultWindowZoom = 0.75f;

// The Web Management UI's default port. Real hardware uses 80 (see
// firmware/main/homedeck.cpp); a desktop dev machine typically can't
// bind that without root, so the simulator defaults elsewhere, with the
// same override convention as HOMEDECK_SIM_ZOOM above.
static constexpr uint16_t kDefaultWebPort = 8080;

namespace {

float ResolveWindowZoom() {
    const char* override_str = std::getenv("HOMEDECK_SIM_ZOOM");
    if (override_str == nullptr) {
        return kDefaultWindowZoom;
    }
    char* end = nullptr;
    float value = std::strtof(override_str, &end);
    if (end == override_str || value <= 0.0f) {
        return kDefaultWindowZoom;
    }
    return value;
}

uint16_t ResolveWebPort() {
    const char* override_str = std::getenv("HOMEDECK_SIM_WEB_PORT");
    if (override_str == nullptr) {
        return kDefaultWebPort;
    }
    char* end = nullptr;
    long value = std::strtol(override_str, &end, 10);
    if (end == override_str || value <= 0 || value > 65535) {
        return kDefaultWebPort;
    }
    return static_cast<uint16_t>(value);
}

// Temporary test-only wiring proving LowBatteryMonitor/NotificationBanner
// end to end - HostBatteryReader is a fixed-then-adjustable mock (see
// platform/host/battery_reader.h) that never naturally crosses the low-
// battery threshold on its own, so there's no other way to see the real
// notification flow run in the simulator. Removed once a real widget
// (weather) or some other real trigger exists to exercise this
// naturally; kept out of LowBatteryMonitor itself so real product code
// stays free of throwaway test scaffolding, the same reasoning
// CreateTestNavButton above already follows.
void OnTestLowBatteryClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetPercent(10);
}

void CreateTestLowBatteryButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_add_event_cb(button, OnTestLowBatteryClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger low battery");
}

// Temporary test-only wiring proving GET /api/diagnostics' external-power
// field end to end - HostBatteryReader's external-power flag never
// changes on its own, the same reasoning CreateTestLowBatteryButton
// above already follows. Removed once a real Power Management screen
// exists to exercise this.
void OnTestExternalPowerClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetExternalPowerConnected(!battery_reader->IsExternalPowerConnected());
}

void CreateTestExternalPowerButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -112);
    lv_obj_add_event_cb(button, OnTestExternalPowerClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle external power");
}

// Temporary test-only wiring proving LowBatteryMonitor doesn't fire
// (and GET /api/diagnostics' batteryPresent field reflects reality)
// when no battery is installed - the same reasoning
// CreateTestExternalPowerButton above already follows. Removed once a
// real Power Management screen exists to exercise this.
void OnTestBatteryPresentClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetBatteryPresent(!battery_reader->IsBatteryPresent());
}

void CreateTestBatteryPresentButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -160);
    lv_obj_add_event_cb(button, OnTestBatteryPresentClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle battery present");
}

// Temporary test-only wiring proving the Web UI's OTA page surfaces a
// real upload failure, not just the success path - see
// docs/architecture/simulator.md's OTA mock description. Removed once a
// real Power Management screen (or similar) exists to exercise this.
void OnTestForceOtaFailureClicked(lv_event_t* e) {
    auto* force_failure = static_cast<bool*>(lv_event_get_user_data(e));
    *force_failure = !*force_failure;
}

void CreateTestForceOtaFailureButton(lv_obj_t* parent, bool& force_failure) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -208);
    lv_obj_add_event_cb(button, OnTestForceOtaFailureClicked, LV_EVENT_CLICKED, &force_failure);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle force OTA failure");
}

// Temporary test-only wiring proving the Web UI's new Logs section
// renders real entries without needing multiple simulator restarts to
// accumulate them - see docs/decisions/ADR-0019-structured-logging.md.
// Removed once a real, naturally-occurring event exists to exercise
// this in the simulator (there's no Wi-Fi/mDNS bring-up here to log,
// unlike firmware).
void OnTestLogEntryClicked(lv_event_t* e) {
    auto* logger = static_cast<homedeck::Logger*>(lv_event_get_user_data(e));
    logger->Log(homedeck::LogLevel::kInfo, "simulator", "Test log entry");
}

void CreateTestLogEntryButton(lv_obj_t* parent, homedeck::Logger& logger) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -256);
    lv_obj_add_event_cb(button, OnTestLogEntryClicked, LV_EVENT_CLICKED, &logger);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: write a log entry");
}

// Temporary test-only wiring to reach the Wi-Fi setup screen - the
// simulator has no real SoftAP "not provisioned" path to trigger it
// naturally (see wifi_setup_screen.h), the same reasoning
// CreateTestLowBatteryButton below already follows for LowBatteryMonitor.
// Removed once a real trigger exists.
void OnTestWifiSetupNavClicked(lv_event_t* e) {
    auto* navigation = static_cast<homedeck::Navigation*>(lv_event_get_user_data(e));
    navigation->GoTo("wifi-setup");
}

void CreateTestWifiSetupNavButton(lv_obj_t* parent, homedeck::Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -304);
    lv_obj_add_event_cb(button, OnTestWifiSetupNavClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: go to Wi-Fi setup screen");
}

}  // namespace

// The dashboard shell, StatusBar, and DashboardGrid widget framework -
// see dashboard.md's Status for what's real. Plus a minimal Navigation
// manager and persistent home affordance, proven via a deliberately
// throwaway second screen (see screens/placeholder_screen.h).
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus, ResolveWindowZoom());

    // DashboardScreen (whose StatusBar and ClockWidget members are both
    // ClockTickEvent subscribers) must exist before Clock (the
    // publisher) - Clock publishes one tick immediately at construction
    // (see clock.cpp) so subscribers get a real value right away rather
    // than sitting blank for up to a full tick period; that only
    // reaches anyone if the subscription already exists when it
    // happens.
    homedeck::HostBatteryReader battery_reader;
    homedeck::HostNetworkStatus network_status;
    homedeck::DashboardScreen dashboard(event_bus, battery_reader, network_status);
    homedeck::ClockWidget clock_widget(dashboard.Grid().Container(), event_bus);
    dashboard.Grid().AddWidget(clock_widget);

    homedeck::Navigation navigation("dashboard", dashboard.Root());

    // Touch UI fallback for initial Wi-Fi setup (see
    // docs/architecture/networking.md#initial-wi-fi-provisioning) - built
    // and exercisable here like any other screen, per this project's
    // "simulator as primary UI dev environment" philosophy, even though
    // there's no real esp_wifi call to make on this target.
    homedeck::WifiSetupScreen wifi_setup_screen(
        event_bus, battery_reader, network_status, [](const std::string& ssid, const std::string& password) {
            std::printf("Wi-Fi setup submitted (no-op in simulator): SSID=%s\n", ssid.c_str());
        });
    // No real SoftAP here to derive these from - a fixed placeholder is
    // enough to exercise the layout (see wifi_setup_screen.h's SetApInfo).
    wifi_setup_screen.SetApInfo("HomeDeck-SIM01", "192.168.4.1");
    navigation.Register("wifi-setup", wifi_setup_screen.Root());

    CreateTestWifiSetupNavButton(dashboard.Root(), navigation);
    CreateTestLowBatteryButton(dashboard.Root(), battery_reader);
    CreateTestExternalPowerButton(dashboard.Root(), battery_reader);
    CreateTestBatteryPresentButton(dashboard.Root(), battery_reader);
    // Declared here, not narrower - captured by reference into
    // ota_writer.write_image below, which must stay valid for the
    // server's lifetime.
    bool force_ota_failure = false;
    CreateTestForceOtaFailureButton(dashboard.Root(), force_ota_failure);

    // NotificationBanner must exist before LowBatteryMonitor, which must
    // exist before Clock (the ClockTickEvent publisher) - the same
    // "subscriber before publisher" ordering StatusBar's own comment
    // above already explains, extended one step further: LowBatteryMonitor
    // itself publishes NotificationEvent, so NotificationBanner needs to
    // already be subscribed before that first tick could fire one.
    homedeck::NotificationBanner notification_banner(event_bus);
    homedeck::LowBatteryMonitor low_battery_monitor(event_bus, battery_reader);
    // Same "subscriber before publisher" ordering as LowBatteryMonitor -
    // NetworkStatusMonitor is also a ClockTickEvent subscriber.
    homedeck::NetworkStatusMonitor network_status_monitor(event_bus, network_status);

    homedeck::HostTimeSource time_source;
    homedeck::Clock clock(time_source, event_bus);

    // AdminAuthService's password hash storage (see
    // docs/architecture/web-ui.md#admin-password) - a fixed location
    // under the OS temp directory rather than a fresh one per run, so
    // the simulator's admin password persists across restarts the same
    // way NVS would on real hardware, instead of demanding first-login
    // setup again every time the simulator relaunches.
    std::filesystem::path storage_root = std::filesystem::temp_directory_path() / "homedeck_simulator";
    homedeck::HostSettingsStore settings_store(storage_root);
    homedeck::HostCacheStore cache_store(storage_root);
    homedeck::HostSecretStore secret_store(storage_root);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::Logger logger(storage, time_source);
    CreateTestLogEntryButton(dashboard.Root(), logger);
    // A monotonic clock, not the shared wall-clock time_source above -
    // matches firmware's identical choice (see
    // platform/steady_time_source.h) so AdminAuthService behaves the
    // same on both targets, rather than the simulator's forgiving host
    // wall clock accidentally masking a real firmware-only issue.
    homedeck::SteadyTimeSource auth_time_source;
    homedeck::AdminAuthService admin_auth(storage, auth_time_source);

    // The Web Management UI's server primitive (see
    // docs/architecture/web-ui.md#status) - the built Svelte/Vite
    // scaffold plus admin auth. Real settings/diagnostics pages are
    // still future passes.
    homedeck::HostHttpServer web_server;
    // Read once at startup, not per-request - matches firmware's
    // EMBED_FILES approach (data available for the process's lifetime),
    // see homedeck.cpp's identical wiring and
    // docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage.
    // HOMEDECK_WEBUI_DIR is a source-tree-relative path (webui/dist)
    // baked in by CMakeLists.txt, so this works regardless of the
    // simulator's current working directory when launched.
    std::vector<homedeck::StaticAsset> webui_assets;
    for (const auto& [path, filename, content_type] :
         {std::tuple{"/", "index.html", "text/html"}, std::tuple{"/app.js", "app.js", "text/javascript"},
          std::tuple{"/app.css", "app.css", "text/css"}}) {
        auto content = homedeck::ReadFile(std::filesystem::path(HOMEDECK_WEBUI_DIR) / filename);
        if (content.has_value()) {
            webui_assets.push_back({path, content_type, *content});
        } else {
            std::printf("Warning: could not read %s/%s\n", HOMEDECK_WEBUI_DIR, filename);
        }
    }
    homedeck::ServeStaticFiles(web_server, std::move(webui_assets));
    homedeck::RegisterAdminAuthRoutes(web_server, admin_auth);
    // Diagnostics needs real Storage-resident data to read (see
    // firmware/main/crash_diagnostics.cpp for the real, firmware-only
    // equivalent) - mock values here so the Web UI's diagnostics page is
    // exercisable in dev without real hardware, per
    // docs/architecture/diagnostics.md's "Firmware-only mechanism" note.
    storage.SetSetting("core", "reset_reason", 1, "power-on");
    storage.SetSetting("core", "has_core_dump", 1, "true");
    homedeck::RegisterDiagnosticsRoutes(web_server, storage, admin_auth, battery_reader, logger,
                                         []() -> std::optional<std::string> {
        return std::string(
            "This is a simulator-only stub core dump for Web UI development - "
            "see docs/architecture/diagnostics.md.");
    });
    // No real OTA partition here - matches firmware's 4MB ota_0/ota_1
    // sizing (see docs/decisions/ADR-0017-partition-table.md) so an
    // oversized-upload rejection is exercisable identically on both
    // targets. write_image reads the body (exercising the same read
    // path as firmware) and discards it - no real partition writes, per
    // docs/architecture/simulator.md's OTA mock description.
    homedeck::OtaWriter ota_writer{
        .max_image_size = []() -> size_t { return 4 * 1024 * 1024; },
        .write_image =
            [&force_ota_failure](const std::string& /*image*/) -> bool { return !force_ota_failure; },
        .running_version = []() -> std::string { return "simulator-dev"; },
    };
    homedeck::RegisterOtaRoutes(web_server, admin_auth, battery_reader, ota_writer,
                                 []() { std::printf("OTA reboot requested (no-op in simulator)\n"); });
    // No device-name-changed callback - there's no mDNS to re-announce
    // on the simulator, so a device name change just persists to
    // storage like any other setting (see settings_routes.h's own
    // comment on why the callback is optional).
    homedeck::RegisterSettingsRoutes(web_server, storage, admin_auth);
    uint16_t web_port = ResolveWebPort();
    if (web_server.Start(web_port)) {
        std::printf("Web UI listening on http://localhost:%u/\n", web_port);
        logger.Log(homedeck::LogLevel::kInfo, "web_server", "Listening on port " + std::to_string(web_port));
    } else {
        std::printf("Web UI failed to start on port %u\n", web_port);
        logger.Log(homedeck::LogLevel::kError, "web_server", "Failed to start on port " + std::to_string(web_port));
    }

    ui_task.Run();
}
