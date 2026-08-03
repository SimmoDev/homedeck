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
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "core/admin_auth_service.h"
#include "core/event_bus.h"
#include "core/storage.h"
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
#include "ui/app_core.h"
#include "ui/lvgl_user_activity_source.h"
#include "ui/ui_dispatch.h"
#include "wifi_setup.h"

// The real HomeDeck firmware entry point - the dashboard (see
// docs/architecture/dashboard.md), Navigation and the Wi-Fi setup screen
// (see docs/architecture/ui.md#status), and the Web Management UI (see
// docs/architecture/web-ui.md), all running on-device. See
// docs/architecture/hardware.md#display-driver-strategy for the
// display/touch driver stack this runs on.
// firmware/platform/task.cpp and timer.cpp (FreeRTOS-backed, per
// ADR-0002) exist because Clock needs a working Timer.
namespace {

// webui/dist/{index.html,app.js,app.css} - the built Svelte/Vite bundle,
// linked into the app image via EMBED_FILES (see CMakeLists.txt), not
// the storage FAT partition - see
// docs/decisions/ADR-0025-webui-static-asset-storage.md
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
// meaningful. OtaRebootFn has no failure-reporting contract (the 200
// response is already committed by the time this runs), so a scheduling
// failure here has nothing left to report to - only worth logging.
void ScheduleReboot() {
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = [](void*) { esp_restart(); };
    args.name = "ota_reboot";
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, 500 * 1000) != ESP_OK) {
        printf("ScheduleReboot: failed to schedule the deferred reboot\n");
    }
}

// Passed to RegisterWifiRoutes as its WifiResetFn. Same deferral reason as
// ScheduleReboot() above, but sharper here: esp_wifi_restore() itself (not
// just the later esp_restart()) tears down the STA association - calling
// it synchronously inside the /api/wifi/reset handler severs the very
// TCP connection the 200 response needs to travel back over, before
// httpd_resp_send() can flush it. Confirmed on hardware as the request
// hanging forever (no HTTP response ever arrives, so the Web UI's fetch()
// never resolves - fetch has no built-in timeout), not merely a
// theoretical race.
//
// A reboot afterward isn't an optional nicety here the way it is for OTA
// (upload success doesn't itself disconnect anything - the Web UI could
// legitimately offer a separate confirmed reboot step there). Restoring
// Wi-Fi settings only clears the C6's stored credentials; the device only
// re-evaluates "are credentials stored" and re-enters SoftAP setup inside
// InitWifiAndCheckStoredCredentials(), which runs once at boot. Without
// rebooting, wifi_setup.cpp's own normal (non-setup) reconnect path would
// just keep retrying against the now-empty config indefinitely instead of
// ever reaching SoftAP mode - so this schedules the restore and the
// reboot together, automatically, rather than leaving the reboot to a
// second confirmed Web UI action that could never actually be clicked in
// time regardless (the connection carrying the first response is already
// gone by then).
//
// Returns false if scheduling itself fails (matching WifiResetFn's own
// contract, see wifi_routes.h) - the caller must not report success to
// the Web UI in that case, since neither the credential clear nor the
// reboot will actually happen.
bool ScheduleWifiResetAndReboot() {
    esp_timer_handle_t timer = nullptr;
    esp_timer_create_args_t args = {};
    args.callback = [](void*) {
        esp_wifi_restore();
        esp_restart();
    };
    args.name = "wifi_reset_reboot";
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, 500 * 1000) != ESP_OK) {
        printf("ScheduleWifiResetAndReboot: failed to schedule the deferred Wi-Fi reset/reboot\n");
        return false;
    }
    return true;
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

// Shared by both InitNvs() partitions below - a fresh/corrupt partition
// (no-free-pages, or a version bump) needs one erase-and-retry, not a
// hard failure.
void InitNvsPartition(esp_err_t (*init)(), esp_err_t (*erase)()) {
    esp_err_t result = init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(erase());
        result = init();
    }
    ESP_ERROR_CHECK(result);
}

// General system init required by Wi-Fi (and later, other Core services
// that need NVS/the event loop) - see
// docs/architecture/networking.md#initial-wi-fi-provisioning. Called
// ahead of the dashboard's construction in app_main() (unlike the rest
// of Wi-Fi bring-up, which stays deferred until after first paint) so
// InitWifiAndCheckStoredCredentials() can answer "is setup needed"
// before any real screen is shown - the splash shown just before this
// call is what keeps first paint fast despite that. Also initializes
// FirmwareSecretStore's own dedicated partition (see
// docs/decisions/ADR-0027-secret-store-partition-separation.md) - must
// run before secret_store's first use, same requirement as the default
// partition below.
void InitNvs() {
    InitNvsPartition(&nvs_flash_init, &nvs_flash_erase);
    InitNvsPartition(
        [] { return nvs_flash_init_partition(homedeck::FirmwareSecretStore::kPartitionName); },
        [] { return nvs_flash_erase_partition(homedeck::FirmwareSecretStore::kPartitionName); });
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

// Bridges esp_sntp_config_t's sync_cb (a plain C function pointer, no
// user_data slot to close over anything with) to the Rx8130TimeSource/
// Logger this file already owns - the same file-scope-global exception
// wifi_setup.cpp's own g_state takes, for the identical reason (an
// ESP-IDF callback signature that can't carry context). Both pointers
// are set once, from the single-threaded boot sequence, before Wi-Fi
// (and therefore any possible sync) can occur - not touched again after
// that, so no synchronization is needed the way wifi_setup.cpp's
// multi-writer g_state_mutex is.
homedeck::Rx8130TimeSource* g_time_source_for_sntp = nullptr;
homedeck::Logger* g_logger_for_sntp = nullptr;

// Fires on every sync - the first one after Wi-Fi connects, and every
// periodic resync LwIP's SNTP client runs on its own thereafter (see
// ADR-0028 for why periodic resync, not just a one-shot set, matters
// here). Writing back to the physical RTC on every call, not just the
// first, keeps it correct across a reboot even if this boot's own
// uptime is short.
void OnSntpTimeSync(timeval* tv) {
    if (g_time_source_for_sntp != nullptr) {
        g_time_source_for_sntp->SetTime(tv->tv_sec);
    }
    if (g_logger_for_sntp != nullptr) {
        g_logger_for_sntp->Log(homedeck::LogLevel::kInfo, "time_sync", "RTC corrected via SNTP");
    }
}

// See ADR-0028 for why SNTP over a manual-set Web UI field. pool.ntp.org
// is a public NTP pool with no configuration needed - this project has
// no per-user NTP-server setting, the same "no premature abstraction"
// reasoning weather.md's direct Open-Meteo default already follows.
// Fire-and-forget: init with wait_for_sync=false (ESP_NETIF_SNTP_DEFAULT_CONFIG's
// own default) so this never blocks app_main() waiting on a sync that
// might not come (e.g. Wi-Fi connected but no internet route) -
// OnSntpTimeSync() above runs whenever a sync eventually succeeds,
// however long that takes, with no timeout of its own.
void StartTimeSync(homedeck::Rx8130TimeSource& time_source, homedeck::Logger& logger) {
    g_time_source_for_sntp = &time_source;
    g_logger_for_sntp = &logger;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = OnSntpTimeSync;
    esp_netif_sntp_init(&config);
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

// Static asset serving stays here rather than in AppCore - the embedded
// EMBED_FILES byte arrays above are firmware-only (the simulator reads
// webui/dist/ off disk instead, see simulator/main.cpp), so this is the
// one piece of Web UI wiring that's genuinely not shared.
void ServeEmbeddedWebUi(homedeck::HttpServer& web_server) {
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
}

// The first thing app_main() does after the boot banner - display bring-up
// has no long-lived object of its own worth returning (bsp_display_start()'s
// own lv_display_t* is never referenced again once this succeeds), so this
// is void rather than needing a caller to hold onto anything. Halts forever
// rather than returning on failure, the same as the inline version this
// replaces - there's no meaningful degraded mode without a display, and
// nothing later in app_main() could safely proceed without one.
void InitDisplayOrHalt() {
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
}

// `event_bus` is constructed by the caller, not here - EventBus holds a
// std::mutex internally, so it can't be constructed in one function and
// returned by value into another the way the splash screen's lv_obj_t*
// pointer below can. Returns the splash object so app_main() can delete it
// once AppCore's construction (and the real first screen it builds) has
// happened - this function has no way to know when that is.
lv_obj_t* InitEventBusAndShowSplash(homedeck::EventBus& event_bus) {
    // bsp_display_start() (already called by InitDisplayOrHalt() above)
    // already spawned its own dedicated taskLVGL (see esp_lvgl_port.c),
    // running its own lv_timer_handler() loop concurrently with app_main()
    // from this point on - so, unlike the simulator's equivalent call (see
    // ui_task.cpp), this lv_* call needs the same lock every other direct
    // LVGL call from app_main() already takes (splash screen, dashboard
    // construction, below).
    bsp_display_lock(0);
    homedeck::InitUiDispatchQueue();
    bsp_display_unlock();

    event_bus.SetUiDispatcher(homedeck::PostToUiThread);

    bsp_display_lock(0);
    lv_obj_t* splash = ShowSplashScreen();
    bsp_display_unlock();
    return splash;
}

// Continues from InitWifiAndCheckStoredCredentials() (already run by the
// time app_main() calls this) - blocks until connected, either immediately
// (stored credentials), until a computer/phone completes SoftAP setup, or
// until the Touch UI fallback screen (already showing, if wifi_check found
// no stored credentials) submits credentials directly. wifi_setup.cpp has
// no LVGL/Navigation dependency of its own, so reaching the UI happens
// through these three callbacks instead - each routed through
// PostToUiThread (see ui/ui_dispatch.h), since they fire from
// ConnectToWifi()'s own task, not the UI task (see ADR-0011).
void BlockUntilWifiConnected(homedeck::AppCore& app_core) {
    homedeck::WifiUiCallbacks wifi_ui_callbacks;
    wifi_ui_callbacks.on_setup_needed = [&app_core](const std::string& ap_ssid, const std::string& ap_ip,
                                                     uint16_t port) {
        homedeck::PostToUiThread([&app_core, ap_ssid, ap_ip, port]() {
            app_core.GetWifiSetupScreen().SetApInfo(ap_ssid, ap_ip, port);
            app_core.GetNavigation().GoTo("wifi-setup");
        });
    };
    wifi_ui_callbacks.on_connected = [&app_core]() {
        homedeck::PostToUiThread([&app_core]() { app_core.GetNavigation().GoHome(); });
    };
    wifi_ui_callbacks.on_connect_failed = [&app_core]() {
        homedeck::PostToUiThread([&app_core]() {
            app_core.GetWifiSetupScreen().SetConnectError("Couldn't connect - check the password and try again.");
        });
    };
    homedeck::ConnectToWifi(wifi_ui_callbacks);
    printf("Wi-Fi connected\n");
    app_core.GetLogger().Log(homedeck::LogLevel::kInfo, "wifi", "Connected to Wi-Fi");
}

// Everything that only becomes possible/meaningful once Wi-Fi is actually
// connected - time sync, mDNS self-advertisement, and finally starting the
// Web Management UI itself (see ServeEmbeddedWebUi(), already called before
// this - only the actual accept-connections Start() call waits for Wi-Fi).
// Also where the OTA rollback confirmation lives: a real, meaningful "this
// boot actually worked" checkpoint belongs after the boot sequence has
// gotten this far, not any earlier.
void FinalizeBootAfterWifiConnected(homedeck::Rx8130TimeSource& time_source, homedeck::FirmwareHttpServer& web_server,
                                     homedeck::AppCore& app_core) {
    // See ADR-0028 - corrects the RTC's previously-never-calibrated time
    // once Wi-Fi (and, implicitly, internet reachability) is available.
    StartTimeSync(time_source, app_core.GetLogger());

    // "homedeck" by default, or whatever a user has set via the Web
    // UI's Settings page (see docs/decisions/ADR-0023-settings-backup-api.md).
    std::string device_name = ResolveDeviceName(app_core.GetStorage());
    RegisterMdns(device_name, app_core.GetLogger());

    // Started only now, after Wi-Fi connects and wifi_setup.cpp's own
    // temporary SoftAP-setup server has already stopped, so there's no
    // port/lifecycle overlap between the two. Routes were already
    // registered against web_server inside AppCore's constructor.
    if (web_server.Start(80)) {
        printf("Web UI listening on port 80\n");
        app_core.GetLogger().Log(homedeck::LogLevel::kInfo, "web_server", "Listening on port 80");
    } else {
        printf("Web UI failed to start\n");
        app_core.GetLogger().Log(homedeck::LogLevel::kError, "web_server", "Failed to start");
    }

    // A real, meaningful "this boot actually worked" checkpoint - see
    // sdkconfig.defaults' CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE comment.
    // ESP_OK is a no-op result (rollback disabled, or this wasn't a
    // pending-verify boot) rather than success-of-an-action, so this logs
    // rather than ESP_ERROR_CHECK-ing - a real failure here means the
    // bootloader could still roll back a boot that actually worked, worth
    // a trace, not worth crashing an otherwise-healthy device over.
    esp_err_t rollback_result = esp_ota_mark_app_valid_cancel_rollback();
    if (rollback_result != ESP_OK) {
        app_core.GetLogger().Log(homedeck::LogLevel::kWarning, "ota",
                                  std::string("Failed to cancel rollback: ") + esp_err_to_name(rollback_result));
    }
}

}  // namespace

extern "C" void app_main(void) {
    PrintBootBanner();
    InitDisplayOrHalt();

    homedeck::EventBus event_bus;
    lv_obj_t* splash = InitEventBusAndShowSplash(event_bus);

    // The BSP already brought up the shared I2C bus for display/touch -
    // reused here rather than creating a second, conflicting bus on the
    // same physical pins.
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    homedeck::Ina226BatteryReader battery_reader(i2c_bus);
    homedeck::Rx8130TimeSource time_source(i2c_bus);
    homedeck::FirmwareNetworkStatus network_status;
    homedeck::FirmwareAudioOutput audio_output;
    // Safe to construct here, ahead of AppCore below - neither
    // constructor touches LVGL/BSP state itself, only their later-called
    // methods do, and those only ever run from PowerManager's own
    // SubscribeUi tick (see ui/lvgl_user_activity_source.h), already
    // UI-thread-safe by then.
    homedeck::FirmwareDisplayBrightness display_brightness;
    homedeck::LvglUserActivitySource user_activity_source;

    InitNvs();

    // Requires nvs_flash_init() above to have already run (see
    // platform/firmware/settings_store.h's and secret_store.h's own
    // "Requires nvs_flash_init()" comments).
    homedeck::FirmwareSettingsStore settings_store;
    homedeck::FirmwareCacheStore cache_store;
    homedeck::FirmwareSecretStore secret_store;
    // Outlives AppCore's own OpenMeteoWeatherProvider poll Task, which
    // holds a reference to it for the rest of app_main's life.
    homedeck::FirmwareHttpClient http_client;
    // Declared here (not narrower) so it stays alive for the rest of
    // app_main's life, which never returns.
    homedeck::FirmwareHttpServer web_server;
    ServeEmbeddedWebUi(web_server);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    homedeck::WifiCredentialsCheck wifi_check = homedeck::InitWifiAndCheckStoredCredentials(network_status);

    // Builds the full shared object graph (dashboard, widgets,
    // notifications, PowerManager, Web UI routes, ...) - see
    // ui/app_core.h. Still inside the bsp_display_lock() span below -
    // its constructor creates LVGL objects directly, the same
    // requirement every other direct LVGL call in this function already
    // has (see ADR-0011).
    bsp_display_lock(0);
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
            .time_source = time_source,
            .wifi_submit =
                [](const std::string& ssid, const std::string& password) {
                    return homedeck::ApplyWifiCredentials(ssid, password);
                },
            .wifi_reset =
                [ap_ssid = wifi_check.ap_ssid]() -> std::optional<std::string> {
                    if (!ScheduleWifiResetAndReboot()) {
                        return std::nullopt;
                    }
                    return ap_ssid;
                },
            .ota_writer = BuildOtaWriter(),
            .ota_reboot = ScheduleReboot,
            .read_core_dump =
                []() -> std::optional<std::string> {
                    // Real flash read against the coredump partition (see
                    // docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md)
                    // - mirrors crash_diagnostics.cpp's own
                    // esp_core_dump_image_check() gate rather than
                    // trusting esp_core_dump_image_get() alone to signal
                    // absence.
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
                },
        });
    // wifi_check (above) already knows whether setup is needed, so the
    // correct screen loads immediately - never the dashboard first,
    // followed a moment later by a redirect once ConnectToWifi() below
    // gets around to it.
    if (!wifi_check.has_stored_credentials) {
        app_core.GetWifiSetupScreen().SetApInfo(wifi_check.ap_ssid, wifi_check.ap_ip);
        app_core.GetNavigation().GoTo("wifi-setup");
    }
    lv_obj_delete(splash);
    bsp_display_unlock();
    if (wifi_check.has_stored_credentials) {
        printf("Dashboard loaded\n");
    } else {
        printf("Wi-Fi setup screen loaded\n");
    }

    // Every ClockTickEvent subscriber above is already constructed, so
    // this can't miss the first tick regardless of the order they were
    // built in - see clock.h's own comment on why this is a separate
    // call.
    app_core.Start();

    homedeck::LogCrashDiagnostics(app_core.GetStorage());

    // See settings_routes.h/AppCore::SetOnDeviceNameValidate()/
    // SetOnDeviceNameCommitted()'s own comments - deferred until after
    // AppCore's construction so the committed callback can safely
    // reference GetLogger(). Split across two calls (not one, unlike
    // before) specifically so the live mDNS re-announce below only ever
    // runs once Storage::SetSetting() has actually persisted the new
    // name - never on a name this validator itself already rejected, and
    // never left applied against a value a later storage-write failure
    // didn't actually save.
    app_core.SetOnDeviceNameValidate(
        [](const std::string& value) -> bool { return IsValidHostnameLabel(value); });
    app_core.SetOnDeviceNameCommitted([&app_core](const std::string& value) {
        if (mdns_hostname_set(value.c_str()) == ESP_OK) {
            printf("mDNS re-announced as %s.local\n", value.c_str());
            app_core.GetLogger().Log(homedeck::LogLevel::kInfo, "mdns", "Re-announced as " + value + ".local");
        } else {
            app_core.GetLogger().Log(homedeck::LogLevel::kWarning, "mdns",
                                      "Failed to re-announce as " + value + ".local");
        }
    });

    BlockUntilWifiConnected(app_core);
    FinalizeBootAfterWifiConnected(time_source, web_server, app_core);

    uint32_t heartbeat = 0;
    while (true) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
