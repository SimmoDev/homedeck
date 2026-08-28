#include "debug_panel.h"

#include "core/event_bus.h"
#include "core/ota_routes.h"
#include "core/storage.h"
#include "platform/host/audio_output.h"
#include "platform/host/battery_reader.h"
#include "platform/host/cache_store.h"
#include "platform/host/display_brightness.h"
#include "platform/host/file_backed_store.h"
#include "platform/host/http_client.h"
#include "platform/host/http_server.h"
#include "platform/host/mdns_browser.h"
#include "platform/host/websocket_client.h"
#include "platform/host/network_status.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"
#include "platform/host/ui_task.h"
#include "platform/static_assets.h"
#include "ui/app_core.h"
#include "ui/lvgl_user_activity_source.h"
#include "ui/navigation.h"
#include "ui/screens/wifi_setup_screen.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

// No real SoftAP exists on the simulator - this fixed placeholder stands in
// wherever a real device would report its own MAC-derived AP SSID (the
// Touch UI fallback screen via SetApInfo() below, and the Web UI's
// Wi-Fi-reset diagnostic aid via AppCore::Dependencies::wifi_reset), so
// both surfaces show the same value rather than two different fake ones.
static constexpr const char* kFakeApSsid = "HomeDeck-SIM01";

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

}  // namespace

// The simulator entry point - platform backends and debug-only wiring
// live here; the dashboard, Navigation, and every other shared Core/UI
// object are built by AppCore (see ui/app_core.h), the same class
// firmware/main/homedeck.cpp's app_main() constructs.
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus, ResolveWindowZoom());

    homedeck::HostBatteryReader battery_reader;
    homedeck::HostNetworkStatus network_status;
    homedeck::HostAudioOutput audio_output;
    // Safe to construct here - UiTask's constructor above already ran
    // lv_init(), and neither constructor otherwise touches LVGL/BSP
    // state beyond that (see ui/lvgl_user_activity_source.h).
    homedeck::HostDisplayBrightness display_brightness;
    homedeck::LvglUserActivitySource real_user_activity_source;
    homedeck::sim::DebugOverridableUserActivitySource user_activity_source(real_user_activity_source);

    // HostSettingsStore/HostSecretStore are plain filesystem paths - no
    // OS-init constraint like firmware's nvs_flash_init() to order
    // around here.
    std::filesystem::path storage_root = std::filesystem::temp_directory_path() / "homedeck_simulator";
    homedeck::HostSettingsStore settings_store(storage_root);
    homedeck::HostCacheStore cache_store(storage_root);
    homedeck::HostSecretStore secret_store(storage_root);
    homedeck::HostHttpClient http_client;
    // The Web Management UI's server primitive (see
    // docs/architecture/web-ui.md#status) - the built Svelte/Vite
    // scaffold plus admin auth, settings, and diagnostics.
    homedeck::HostHttpServer web_server;
    homedeck::HostMdnsBrowser mdns_browser;
    homedeck::HostTimeSource time_source;

    // Declared here, not narrower - captured by reference into
    // ota_writer.write_image below, which must stay valid for the
    // server's lifetime.
    bool force_ota_failure = false;
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

    // Builds the full shared object graph (dashboard, widgets,
    // notifications, PowerManager, Web UI routes, ...) - see
    // ui/app_core.h.
    homedeck::AppCore app_core(
        event_bus,
        {
            .battery_reader = battery_reader,
            .network_status = network_status,
            .audio_output = audio_output,
            .display_brightness = display_brightness,
            .user_activity_source = user_activity_source,
            .settings_store = settings_store,
            .cache_store = cache_store,
            .secret_store = secret_store,
            .http_client = http_client,
            .http_server = web_server,
            .make_websocket_client = [] { return std::make_unique<homedeck::HostWebSocketClient>(); },
            .mdns_browser = mdns_browser,
            .time_source = time_source,
            .wifi_submit =
                [](const std::string& ssid, const std::string& /*password*/) {
                    // No real esp_wifi to validate against here, but the
                    // same empty-SSID rejection ApplyWifiCredentials
                    // enforces on firmware, so the Touch UI's error-
                    // display path is exercisable in the simulator too.
                    if (ssid.empty()) {
                        return false;
                    }
                    std::printf("Wi-Fi setup submitted (no-op in simulator): SSID=%s\n", ssid.c_str());
                    return true;
                },
            .wifi_reset =
                []() -> std::optional<std::string> {
                    // Firmware schedules esp_wifi_restore() + esp_restart()
                    // together, deferred past the HTTP response being sent
                    // (see homedeck.cpp's ScheduleWifiResetAndReboot() for
                    // why a reboot is required here, not optional) - no
                    // real Wi-Fi or reboot to simulate here, so this is a
                    // pure no-op, returning the same fixed placeholder SSID
                    // SetApInfo() below gives the Touch UI fallback screen.
                    std::printf("Wi-Fi reset + reboot requested (no-op in simulator)\n");
                    return std::string(kFakeApSsid);
                },
            .ota_writer = ota_writer,
            .ota_reboot = []() { std::printf("OTA reboot requested (no-op in simulator)\n"); },
            .read_core_dump =
                []() -> std::optional<std::string> {
                    return std::string(
                        "This is a simulator-only stub core dump for Web UI development - "
                        "see docs/architecture/diagnostics.md.");
                },
            // No device-name callbacks - there's no mDNS to re-announce
            // and no hostname rules to check on the simulator, so a
            // device name change just persists to storage like any other
            // setting (see AppCore::SetOnDeviceNameValidate()/
            // SetOnDeviceNameCommitted()'s own comments on why both are
            // optional).
        });

    // No real SoftAP here to derive these from - a fixed placeholder is
    // enough to exercise the layout (see wifi_setup_screen.h's SetApInfo).
    app_core.GetWifiSetupScreen().SetApInfo(kFakeApSsid, "192.168.4.1");
    homedeck::sim::CreateTestBackToDashboardButton(app_core.GetWifiSetupScreen().Root(), app_core.GetNavigation());

    lv_obj_t* test_button_panel = homedeck::sim::CreateTestButtonPanel(app_core.GetDashboard().Root());
    homedeck::sim::CreateTestWifiSetupNavButton(test_button_panel, app_core.GetNavigation());
    homedeck::sim::CreateTestWifiDisconnectButton(test_button_panel, network_status);
    homedeck::sim::CreateTestPlayToneButton(test_button_panel, audio_output);
    homedeck::sim::CreateTestLowBatteryButton(test_button_panel, battery_reader);
    homedeck::sim::CreateTestCriticalBatteryButton(test_button_panel, battery_reader);
    homedeck::sim::CreateTestExternalPowerButton(test_button_panel, battery_reader);
    homedeck::sim::CreateTestBatteryPresentButton(test_button_panel, battery_reader);
    homedeck::sim::CreateTestForceOtaFailureButton(test_button_panel, force_ota_failure);
    homedeck::sim::CreateTestTriggerIdleButton(test_button_panel, user_activity_source);
    homedeck::sim::CreateTestTriggerSleepingButton(test_button_panel, user_activity_source);
    homedeck::sim::CreateTestTriggerActiveButton(test_button_panel, user_activity_source);
    homedeck::sim::CreateTestLogEntryButton(test_button_panel, app_core.GetLogger());

    // Every ClockTickEvent subscriber above is already constructed, so
    // this can't miss the first tick regardless of the order they were
    // built in - see clock.h's own comment on why this is a separate
    // call.
    app_core.Start();

    // Diagnostics needs real Storage-resident data to read (see
    // firmware/main/crash_diagnostics.cpp for the real, firmware-only
    // equivalent) - mock values here so the Web UI's diagnostics page is
    // exercisable in dev without real hardware, per
    // docs/architecture/diagnostics.md's "Firmware-only mechanism" note.
    // Read at request time by the diagnostics route AppCore's
    // constructor already registered, so setting these after
    // construction (but before the server starts below) is enough.
    if (!app_core.GetStorage().SetSetting("core", "reset_reason", 1, "power-on")) {
        std::cerr << "simulator: failed to seed mock reset_reason setting\n";
    }
    if (!app_core.GetStorage().SetSetting("core", "has_core_dump", 1, "true")) {
        std::cerr << "simulator: failed to seed mock has_core_dump setting\n";
    }

    // Read once at startup, not per-request - matches firmware's
    // EMBED_FILES approach (data available for the process's lifetime),
    // see homedeck.cpp's identical wiring and
    // docs/decisions/ADR-0025-webui-static-asset-storage.md.
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

    uint16_t web_port = ResolveWebPort();
    if (web_server.Start(web_port)) {
        std::printf("Web UI listening on http://localhost:%u/\n", web_port);
        app_core.GetLogger().Log(homedeck::LogLevel::kInfo, "web_server",
                                  "Listening on port " + std::to_string(web_port));
    } else {
        std::printf("Web UI failed to start on port %u\n", web_port);
        app_core.GetLogger().Log(homedeck::LogLevel::kError, "web_server",
                                  "Failed to start on port " + std::to_string(web_port));
    }

    ui_task.Run();
}
