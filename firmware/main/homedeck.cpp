#include <cctype>
#include <cstdio>

#include "bsp/m5stack_tab5.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mdns.h"
#include "nvs_flash.h"

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
#include "core/weather_provider.h"
#include "core/weather_routes.h"
#include "crash_diagnostics.h"
#include "platform/firmware/audio_output.h"
#include "platform/firmware/battery_reader.h"
#include "platform/firmware/cache_store.h"
#include "platform/firmware/display_brightness.h"
#include "platform/firmware/http_client.h"
#include "platform/firmware/http_server.h"
#include "platform/firmware/network_status.h"
#include "platform/firmware/secret_store.h"
#include "platform/firmware/settings_store.h"
#include "platform/firmware/time_source.h"
#include "platform/static_assets.h"
#include "platform/steady_time_source.h"
#include "ui/clock_widget.h"
#include "ui/lvgl_user_activity_source.h"
#include "ui/navigation.h"
#include "ui/network_status_widget.h"
#include "ui/notification_banner.h"
#include "ui/notification_widget.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/screens/wifi_setup_screen.h"
#include "ui/ui_dispatch.h"
#include "ui/weather_widget.h"
#include "wifi_setup.h"

// The real HomeDeck firmware entry point - the dashboard (see
// docs/architecture/dashboard.md), Navigation and the Wi-Fi setup screen
// (see docs/architecture/ui.md#status), and the Web Management UI (see
// docs/architecture/web-ui.md), all running on-device. Display and touch
// are confirmed working on this hardware - see
// docs/architecture/hardware.md#display-driver-strategy.
// firmware/platform/task.cpp and timer.cpp (FreeRTOS-backed, per
// ADR-0002) exist because Clock needs a working Timer.
namespace {

// webui/dist/{index.html,app.js,app.css} - the built Svelte/Vite bundle,
// linked into the app image via EMBED_FILES (see CMakeLists.txt), not
// the storage FAT partition - see
// docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage
// for why. Symbol names are derived from each embedded file's basename by
// ESP-IDF's build system (dots become underscores), not chosen here.
extern const uint8_t webui_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t webui_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t webui_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t webui_app_js_end[] asm("_binary_app_js_end");
extern const uint8_t webui_app_css_start[] asm("_binary_app_css_start");
extern const uint8_t webui_app_css_end[] asm("_binary_app_css_end");

// The first thing app_main() does, before anything else can fail and
// leave no trace of what was even running.
void PrintBootBanner() {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("HomeDeck firmware boot\n");
    printf("  IDF version:  %s\n", esp_get_idf_version());
    printf("  Chip:         %s, cores: %d, revision: v%d.%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    printf("  Free heap:    %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Free PSRAM:   %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Passed to RegisterOtaRoutes as its OtaRebootFn - esp_restart() can't
// be called directly from the /api/ota/reboot handler, since the
// handler still has to return so its 200 response is actually sent
// first. The delay just needs to clear that write; it isn't otherwise
// meaningful.
void ScheduleReboot() {
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = [](void*) { esp_restart(); };
    args.name = "ota_reboot";
    esp_timer_create(&args, &timer);
    esp_timer_start_once(timer, 500 * 1000);
}

// Shown immediately after display start, before the Wi-Fi credentials
// check below (a real SDIO/RPC round trip to the C6, not instant) - masks
// that latency with something meaningful rather than either an LVGL
// default blank screen or briefly showing the dashboard before knowing
// whether Wi-Fi setup is actually needed. Caller deletes the returned
// object once the real initial screen has loaded.
lv_obj_t* ShowSplashScreen() {
    lv_obj_t* splash = lv_obj_create(nullptr);
    lv_obj_t* label = lv_label_create(splash);
    lv_label_set_text(label, "HomeDeck");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_center(label);
    lv_scr_load(splash);
    return splash;
}

// RFC 1035/6763 label rules for the mDNS hostname a user sets via the
// Web UI's Settings page - checked here rather than left to
// mdns_hostname_set() itself, whose failure mode (silently not
// re-announcing) would otherwise be indistinguishable from any other
// cause at the settings_routes.cpp call site.
bool IsValidHostnameLabel(const std::string& label) {
    if (label.empty() || label.size() > 63) return false;
    if (label.front() == '-' || label.back() == '-') return false;
    for (char c : label) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    }
    return true;
}

// General system init required by Wi-Fi (and later, other Core services
// that need NVS/the event loop) - see
// docs/architecture/networking.md#initial-wi-fi-provisioning. Called
// ahead of the dashboard's construction in app_main() (unlike the rest
// of Wi-Fi bring-up, which stays deferred until after first paint) so
// InitWifiAndCheckStoredCredentials() can answer "is setup needed"
// before any real screen is shown - the splash shown just before this
// call is what keeps first paint fast despite that.
void InitNvs() {
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
}

// Not written back here, only read, so an unset name stays genuinely
// unset rather than getting persisted as a default nobody chose.
std::string ResolveDeviceName(homedeck::Storage& storage) {
    auto device_name_setting = storage.GetSetting(homedeck::AdminAuthService::kModuleId, "device_name");
    return device_name_setting.has_value() ? device_name_setting->value : "homedeck";
}

// Self-advertisement only - see
// docs/architecture/networking.md#lan-discovery. Not the Core mDNS
// *browsing* wrapper that doc also names (for modules discovering
// Kodi/Home Assistant) - that has no real consumer until one of those
// modules exists (M4/M6 respectively), so building it now would be
// exactly the kind of speculative Core abstraction ADR-0006 itself
// rejects. This makes the device reachable at <name>.local instead of
// requiring the serial-logged IP.
void RegisterMdns(const std::string& device_name, homedeck::Logger& logger) {
    esp_err_t mdns_result = mdns_init();
    if (mdns_result == ESP_OK) {
        mdns_hostname_set(device_name.c_str());
        mdns_instance_name_set("HomeDeck");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        printf("mDNS advertising as %s.local\n", device_name.c_str());
        logger.Log(homedeck::LogLevel::kInfo, "mdns", "Advertising as " + device_name + ".local");
    } else {
        printf("mDNS init failed: %s\n", esp_err_to_name(mdns_result));
        logger.Log(homedeck::LogLevel::kError, "mdns",
                    std::string("Init failed: ") + esp_err_to_name(mdns_result));
    }
}

// See docs/decisions/ADR-0005-power-and-sleep-model.md's OTA gate
// decision and core/ota_gate.h.
homedeck::OtaWriter BuildOtaWriter() {
    return homedeck::OtaWriter{
        .max_image_size =
            []() -> size_t {
                const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
                return partition != nullptr ? partition->size : 0;
            },
        .write_image =
            [](const std::string& image) -> bool {
                const esp_partition_t* partition = esp_ota_get_next_update_partition(nullptr);
                if (partition == nullptr) {
                    return false;
                }
                esp_ota_handle_t handle;
                if (esp_ota_begin(partition, image.size(), &handle) != ESP_OK) {
                    return false;
                }
                if (esp_ota_write(handle, image.data(), image.size()) != ESP_OK) {
                    esp_ota_end(handle);
                    return false;
                }
                // esp_ota_set_boot_partition() only runs once esp_ota_end()
                // has validated the image - a bad image must never become
                // bootable.
                if (esp_ota_end(handle) != ESP_OK) {
                    return false;
                }
                return esp_ota_set_boot_partition(partition) == ESP_OK;
            },
        .running_version =
            []() -> std::string {
                const esp_app_desc_t* desc = esp_app_get_description();
                return desc != nullptr ? std::string(desc->version) : std::string("unknown");
            },
    };
}

// The Web Management UI's server primitive (see
// docs/architecture/web-ui.md#status) - the built Svelte/Vite scaffold
// plus admin auth. Called after Wi-Fi connects, and after
// wifi_setup.cpp's own temporary SoftAP-setup server has already
// stopped, so there's no port/lifecycle overlap between the two.
// battery_reader is the same Ina226BatteryReader instance the dashboard
// already reads.
void StartWebServer(homedeck::HttpServer& web_server, homedeck::EventBus& event_bus, homedeck::Storage& storage,
                     homedeck::AdminAuthService& admin_auth, homedeck::BatteryReader& battery_reader,
                     homedeck::Logger& logger, homedeck::HttpClient& http_client,
                     homedeck::OpenMeteoWeatherProvider& weather_provider, homedeck::OtaWriter ota_writer) {
    homedeck::ServeStaticFiles(
        web_server,
        {{"/", "text/html",
          std::string(reinterpret_cast<const char*>(webui_index_html_start),
                       webui_index_html_end - webui_index_html_start)},
         {"/app.js", "text/javascript",
          std::string(reinterpret_cast<const char*>(webui_app_js_start),
                       webui_app_js_end - webui_app_js_start)},
         {"/app.css", "text/css",
          std::string(reinterpret_cast<const char*>(webui_app_css_start),
                       webui_app_css_end - webui_app_css_start)}});
    homedeck::RegisterAdminAuthRoutes(web_server, admin_auth);
    // Real flash read against the coredump partition (see
    // docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md) - mirrors
    // crash_diagnostics.cpp's own esp_core_dump_image_check() gate rather
    // than trusting esp_core_dump_image_get() alone to signal absence.
    homedeck::RegisterDiagnosticsRoutes(web_server, storage, admin_auth, battery_reader, logger,
                                         []() -> std::optional<std::string> {
        if (esp_core_dump_image_check() != ESP_OK) {
            return std::nullopt;
        }
        size_t addr = 0, size = 0;
        if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
            return std::nullopt;
        }
        std::string buffer(size, '\0');
        if (esp_flash_read(esp_flash_default_chip, buffer.data(), addr, size) != ESP_OK) {
            return std::nullopt;
        }
        return buffer;
    });
    homedeck::RegisterOtaRoutes(web_server, event_bus, admin_auth, battery_reader, ota_writer, ScheduleReboot);
    homedeck::RegisterSettingsRoutes(web_server, storage, admin_auth, [&logger](const std::string& value) -> bool {
        if (!IsValidHostnameLabel(value)) {
            return false;
        }
        bool ok = mdns_hostname_set(value.c_str()) == ESP_OK;
        if (ok) {
            printf("mDNS re-announced as %s.local\n", value.c_str());
            logger.Log(homedeck::LogLevel::kInfo, "mdns", "Re-announced as " + value + ".local");
        }
        return ok;
    });
    homedeck::RegisterWeatherRoutes(web_server, http_client, weather_provider, admin_auth);
    if (web_server.Start(80)) {
        printf("Web UI listening on port 80\n");
        logger.Log(homedeck::LogLevel::kInfo, "web_server", "Listening on port 80");
    } else {
        printf("Web UI failed to start\n");
        logger.Log(homedeck::LogLevel::kError, "web_server", "Failed to start");
    }
}

}  // namespace

extern "C" void app_main(void) {
    PrintBootBanner();

    printf("Starting display...\n");
    lv_display_t* display = bsp_display_start();
    if (display == nullptr) {
        printf("bsp_display_start() FAILED - halting\n");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    bsp_display_backlight_on();
    printf("Display started\n");

    homedeck::EventBus event_bus;
    event_bus.SetUiDispatcher(homedeck::PostToUiThread);

    bsp_display_lock(0);
    lv_obj_t* splash = ShowSplashScreen();
    bsp_display_unlock();

    // The BSP already brought up the shared I2C bus for display/touch -
    // reused here rather than creating a second, conflicting bus on the
    // same physical pins.
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    homedeck::Ina226BatteryReader battery_reader(i2c_bus);
    homedeck::Rx8130TimeSource time_source(i2c_bus);
    homedeck::FirmwareNetworkStatus network_status;
    homedeck::FirmwareAudioOutput audio_output;
    // Safe to construct here, ahead of the dashboard below - neither
    // constructor touches LVGL/BSP state itself, only their later-called
    // methods do, and those only ever run from PowerManager's own
    // SubscribeUi tick (see ui/lvgl_user_activity_source.h), already
    // UI-thread-safe by then.
    homedeck::FirmwareDisplayBrightness display_brightness;
    homedeck::LvglUserActivitySource user_activity_source;

    InitNvs();

    // Moved ahead of the dashboard's construction below (unlike Logger
    // further down, which doesn't need to be) so WeatherWidget can be
    // constructed alongside ClockWidget/NetworkStatusWidget - requires
    // nvs_flash_init() above to have already run (see
    // platform/firmware/settings_store.h's and secret_store.h's own
    // "Requires nvs_flash_init()" comments); moving this any earlier,
    // e.g. alongside battery_reader/network_status above, would
    // silently break every nvs_open() call in both backing stores.
    homedeck::FirmwareSettingsStore settings_store;
    homedeck::FirmwareCacheStore cache_store;
    homedeck::FirmwareSecretStore secret_store;
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // Outlives OpenMeteoWeatherProvider's own poll Task, which holds a
    // reference to it for the rest of app_main's life.
    homedeck::FirmwareHttpClient http_client;
    homedeck::OpenMeteoWeatherProvider weather_provider(http_client, storage, event_bus);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    homedeck::WifiCredentialsCheck wifi_check = homedeck::InitWifiAndCheckStoredCredentials(network_status);

    bsp_display_lock(0);
    homedeck::DashboardScreen dashboard(event_bus, battery_reader, network_status);
    homedeck::ClockWidget clock_widget(dashboard.Grid().Container(), event_bus);
    dashboard.Grid().AddWidget(clock_widget);
    homedeck::NetworkStatusWidget network_status_widget(dashboard.Grid().Container(), event_bus, network_status);
    dashboard.Grid().AddWidget(network_status_widget);
    homedeck::WeatherWidget weather_widget(dashboard.Grid().Container(), event_bus, weather_provider);
    dashboard.Grid().AddWidget(weather_widget);
    homedeck::NotificationWidget notification_widget(dashboard.Grid().Container(), event_bus);
    dashboard.Grid().AddWidget(notification_widget);
    // NotificationBanner and NotificationSound must exist before
    // LowBatteryMonitor, which must exist before Clock (the
    // ClockTickEvent publisher) - see simulator/main.cpp's identical
    // ordering note. Unlike the simulator, there's no manual trigger
    // here - Ina226BatteryReader is real, so this only actually fires
    // once the pack genuinely runs low.
    homedeck::NotificationBanner notification_banner(event_bus);
    homedeck::NotificationSound notification_sound(event_bus, audio_output);
    homedeck::LowBatteryMonitor low_battery_monitor(event_bus, battery_reader);
    // Same ordering rule as LowBatteryMonitor above - also a
    // ClockTickEvent subscriber, must exist before Clock. PowerManager's
    // own CriticalBatteryStateChangedEvent subscription is set up in its
    // constructor below, before Clock's first tick, so its position
    // relative to this doesn't matter beyond that.
    homedeck::CriticalBatteryMonitor critical_battery_monitor(event_bus, battery_reader);
    // Same "subscriber before publisher" ordering as LowBatteryMonitor -
    // NetworkStatusMonitor is also a ClockTickEvent subscriber, so it
    // must exist before Clock too (see below).
    homedeck::NetworkStatusMonitor network_status_monitor(event_bus, network_status);
    // Grouped here for readability, not a real ordering dependency on
    // NotificationBanner/NotificationSound the way LowBatteryMonitor has
    // - PowerManager neither consumes nor publishes NotificationEvent,
    // it just shares the same "must exist before Clock" rule every
    // ClockTickEvent subscriber does. A second SteadyTimeSource instance
    // (stateless, see platform/steady_time_source.h) rather than sharing
    // auth_time_source below, which is constructed later and named for
    // AdminAuthService specifically.
    homedeck::SteadyTimeSource power_time_source;
    homedeck::PowerManager power_manager(event_bus, user_activity_source, display_brightness,
                                          power_time_source);
    // Navigation's constructor loads home_screen itself (see
    // ui/navigation.h), so no explicit lv_scr_load() call is needed here.
    // wifi_setup_screen is the Touch UI fallback for initial Wi-Fi setup
    // (see docs/architecture/networking.md#initial-wi-fi-provisioning).
    // Declared here (not narrower) so both stay alive for the rest of
    // app_main's life, which never returns.
    homedeck::Navigation navigation("dashboard", dashboard.Root());
    homedeck::WifiSetupScreen wifi_setup_screen(
        event_bus, battery_reader, network_status, [](const std::string& ssid, const std::string& password) {
            homedeck::ApplyWifiCredentials(ssid, password);
        });
    navigation.Register("wifi-setup", wifi_setup_screen.Root());
    // wifi_check (above) already knows whether setup is needed, so the
    // correct screen loads immediately - never the dashboard first,
    // followed a moment later by a redirect once ConnectToWifi() below
    // gets around to it.
    if (!wifi_check.has_stored_credentials) {
        wifi_setup_screen.SetApInfo(wifi_check.ap_ssid, wifi_check.ap_ip);
        navigation.GoTo("wifi-setup");
    }
    lv_obj_delete(splash);
    bsp_display_unlock();
    if (wifi_check.has_stored_credentials) {
        printf("Dashboard loaded\n");
    } else {
        printf("Wi-Fi setup screen loaded\n");
    }

    // Clock (the ClockTickEvent publisher) must exist after the
    // dashboard (the subscriber) is already listening - see
    // simulator/main.cpp's identical ordering note. Clock publishes one
    // tick immediately at construction so subscribers get a real value
    // right away rather than sitting blank for up to a full tick period.
    homedeck::Clock clock(time_source, event_bus);

    homedeck::LogCrashDiagnostics(storage);

    // Constructed here, ahead of Wi-Fi/mDNS below, so Logger - built on
    // Storage (constructed earlier above, see its own comment there),
    // see docs/decisions/ADR-0019-structured-logging.md - can record
    // those as real boot-sequence events rather than starting its
    // persisted log only once the Web UI's own setup begins. See
    // docs/architecture/web-ui.md#admin-password for why the password
    // hash storage this also backs stays plain NVS/FAT for now, per
    // ADR-0018's staged security model. The password hash itself goes
    // through secret_store, not settings_store - see
    // docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface.
    homedeck::Logger logger(storage, time_source);

    // Continues from InitWifiAndCheckStoredCredentials() above - blocks
    // until connected, either immediately (stored credentials), until a
    // phone/laptop completes SoftAP setup, or until the Touch UI fallback
    // screen (already showing, if wifi_check found no stored credentials)
    // submits credentials directly. wifi_setup.cpp has no LVGL/Navigation
    // dependency of its own, so reaching the UI happens through these two
    // callbacks instead - each routed through PostToUiThread (see
    // ui/ui_dispatch.h), since they fire from ConnectToWifi()'s own
    // task, not the UI task (see ADR-0011).
    homedeck::WifiUiCallbacks wifi_ui_callbacks;
    wifi_ui_callbacks.on_setup_needed = [&navigation, &wifi_setup_screen](const std::string& ap_ssid,
                                                                            const std::string& ap_ip) {
        homedeck::PostToUiThread([&navigation, &wifi_setup_screen, ap_ssid, ap_ip]() {
            wifi_setup_screen.SetApInfo(ap_ssid, ap_ip);
            navigation.GoTo("wifi-setup");
        });
    };
    wifi_ui_callbacks.on_connected = [&navigation]() {
        homedeck::PostToUiThread([&navigation]() { navigation.GoHome(); });
    };
    homedeck::ConnectToWifi(wifi_ui_callbacks);
    printf("Wi-Fi connected\n");
    logger.Log(homedeck::LogLevel::kInfo, "wifi", "Connected to Wi-Fi");

    // "homedeck" by default, or whatever a user has set via the Web
    // UI's Settings page (see docs/decisions/ADR-0023-settings-backup-api.md).
    std::string device_name = ResolveDeviceName(storage);
    RegisterMdns(device_name, logger);

    // A monotonic clock, not the shared wall-clock time_source above -
    // see platform/steady_time_source.h for why session expiry can't
    // trust Rx8130TimeSource yet.
    homedeck::SteadyTimeSource auth_time_source;
    homedeck::AdminAuthService admin_auth(storage, auth_time_source);

    // Declared here (not in a narrower scope) so it stays alive for the
    // rest of app_main's life, which never returns.
    homedeck::FirmwareHttpServer web_server;
    StartWebServer(web_server, event_bus, storage, admin_auth, battery_reader, logger, http_client, weather_provider,
                   BuildOtaWriter());

    // A real, meaningful "this boot actually worked" checkpoint - see
    // sdkconfig.defaults' CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE comment.
    // No-op when rollback is disabled or this isn't a pending-verify
    // boot.
    esp_ota_mark_app_valid_cancel_rollback();

    uint32_t heartbeat = 0;
    while (true) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
