#include <cstdio>

#include "bsp/m5stack_tab5.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mdns.h"
#include "nvs_flash.h"

#include "core/admin_auth_service.h"
#include "core/clock.h"
#include "core/diagnostics_routes.h"
#include "core/event_bus.h"
#include "core/low_battery_monitor.h"
#include "crash_diagnostics.h"
#include "platform/firmware/battery_reader.h"
#include "platform/firmware/cache_store.h"
#include "platform/firmware/http_server.h"
#include "platform/firmware/secret_store.h"
#include "platform/firmware/settings_store.h"
#include "platform/firmware/time_source.h"
#include "platform/static_assets.h"
#include "platform/steady_time_source.h"
#include "ui/notification_banner.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/widget.h"
#include "wifi_setup.h"

// The real HomeDeck dashboard, running on-device - see docs/roadmap.md's
// M1 "Basic LVGL application running on-device" item. Display and touch
// are confirmed working on this hardware - see
// docs/architecture/hardware.md#display-driver-strategy.
//
// Deliberately out of scope here: Navigation, the home affordance, and
// any second screen - the dashboard is loaded directly as the only
// screen, matching this step's scope in docs/roadmap.md.
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

// Mirrors the exact lv_async_call()-based hand-off UiTask uses for the
// simulator (src/ui/ui_task.cpp) - this is core LVGL API, not backend-
// specific, so the same mechanism applies whether LVGL is being driven
// by SDL2 or (as here) espressif/m5stack_tab5's BSP.
void RunAndDelete(void* user_data) {
    auto* fn = static_cast<std::function<void()>*>(user_data);
    (*fn)();
    delete fn;
}

// Temporary, throwaway - proves DashboardGrid's mixed-span placement
// (see docs/architecture/dashboard.md#widget-system) renders correctly
// on the real panel, not just the simulator - simulator and real-hardware
// rendering can differ (font legibility, scroll behavior), so this
// framework's actual rendering deserves its own real check. Mirrors
// simulator/widgets/placeholder_widget.h's role
// exactly, kept inline here rather than as a shared file since both
// copies are removed once a real widget (weather) exists.
class TestWidget : public homedeck::Widget {
public:
    TestWidget(lv_obj_t* parent, const char* text, int column_span = 1, int row_span = 1)
        : column_span_(column_span), row_span_(row_span) {
        root_ = lv_obj_create(parent);
        lv_obj_set_style_pad_all(root_, 8, 0);
        lv_obj_t* label = lv_label_create(root_);
        lv_label_set_text(label, text);
        lv_obj_center(label);
    }

    lv_obj_t* Root() const override { return root_; }
    int ColumnSpan() const override { return column_span_; }
    int RowSpan() const override { return row_span_; }

private:
    lv_obj_t* root_;
    int column_span_;
    int row_span_;
};

}  // namespace

extern "C" void app_main(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("HomeDeck firmware boot\n");
    printf("  IDF version:  %s\n", esp_get_idf_version());
    printf("  Chip:         %s, cores: %d, revision: v%d.%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    printf("  Free heap:    %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Free PSRAM:   %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

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
    event_bus.SetUiDispatcher([](std::function<void()> fn) {
        auto* heap_fn = new std::function<void()>(std::move(fn));
        lv_async_call(RunAndDelete, heap_fn);
    });

    // The BSP already brought up the shared I2C bus for display/touch -
    // reused here rather than creating a second, conflicting bus on the
    // same physical pins.
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    homedeck::Ina226BatteryReader battery_reader(i2c_bus);
    homedeck::Rx8130TimeSource time_source(i2c_bus);

    bsp_display_lock(0);
    homedeck::DashboardScreen dashboard(event_bus, battery_reader);
    // Same mixed-span demo as simulator/main.cpp - see TestWidget's own
    // comment above for why this exists on firmware too.
    TestWidget widget_a(dashboard.Grid().Container(), "Widget A", /*column_span=*/2);
    TestWidget widget_b(dashboard.Grid().Container(), "Widget B", /*column_span=*/2,
                         /*row_span=*/2);
    TestWidget widget_c(dashboard.Grid().Container(), "Widget C");
    TestWidget widget_d(dashboard.Grid().Container(), "Widget D");
    dashboard.Grid().AddWidget(widget_a);
    dashboard.Grid().AddWidget(widget_b);
    dashboard.Grid().AddWidget(widget_c);
    dashboard.Grid().AddWidget(widget_d);
    // NotificationBanner must exist before LowBatteryMonitor, which must
    // exist before Clock (the ClockTickEvent publisher) - see
    // simulator/main.cpp's identical ordering note. Unlike the
    // simulator, there's no manual trigger here - Ina226BatteryReader is
    // real, so this only actually fires once the pack genuinely runs low.
    homedeck::NotificationBanner notification_banner(event_bus);
    homedeck::LowBatteryMonitor low_battery_monitor(event_bus, battery_reader);
    lv_scr_load(dashboard.Root());
    bsp_display_unlock();
    printf("Dashboard loaded\n");

    // Clock (the ClockTickEvent publisher) must exist after the
    // dashboard (the subscriber) is already listening - see
    // simulator/main.cpp's identical ordering note. Clock publishes one
    // tick immediately at construction so the display doesn't sit on
    // LVGL's placeholder text until the first periodic tick.
    homedeck::Clock clock(time_source, event_bus);

    // General system init required by Wi-Fi (and later, other Core
    // services that need NVS/the event loop) - see
    // docs/architecture/networking.md#initial-wi-fi-provisioning.
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    homedeck::LogCrashDiagnostics();

    // Blocks until connected - either immediately (stored credentials) or
    // until a phone/laptop completes SoftAP setup. The dashboard is
    // already on-screen and live (RTC-backed Clock, not network-backed)
    // by this point, so this doesn't delay first paint.
    homedeck::ConnectToWifi();
    printf("Wi-Fi connected\n");

    // Self-advertisement only - see
    // docs/architecture/networking.md#lan-discovery. Not the Core mDNS
    // *browsing* wrapper that doc also names (for modules discovering
    // Kodi/Home Assistant) - that has no real consumer until one of
    // those modules exists (M4/M6 respectively), so building it now
    // would be exactly the kind of speculative Core abstraction
    // ADR-0006 itself rejects. This just makes the device reachable as
    // homedeck.local instead of requiring the serial-logged IP.
    esp_err_t mdns_result = mdns_init();
    if (mdns_result == ESP_OK) {
        mdns_hostname_set("homedeck");
        mdns_instance_name_set("HomeDeck");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        printf("mDNS advertising as homedeck.local\n");
    } else {
        printf("mDNS init failed: %s\n", esp_err_to_name(mdns_result));
    }

    // AdminAuthService's password hash storage (see
    // docs/architecture/web-ui.md#admin-password) - plain NVS/FAT for
    // now, per ADR-0018's staged security model
    // (docs/decisions/ADR-0018-staged-security-hardening.md). The
    // password hash itself goes through secret_store, not
    // settings_store - see
    // docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface.
    // Declared here (not narrower) for the same reason web_server is
    // below - it must stay alive for the rest of app_main's life.
    homedeck::FirmwareSettingsStore settings_store;
    homedeck::FirmwareCacheStore cache_store;
    homedeck::FirmwareSecretStore secret_store;
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // A monotonic clock, not the shared wall-clock time_source above -
    // see platform/steady_time_source.h for why session expiry can't
    // trust Rx8130TimeSource yet.
    homedeck::SteadyTimeSource auth_time_source;
    homedeck::AdminAuthService admin_auth(storage, auth_time_source);

    // The Web Management UI's server primitive (see
    // docs/architecture/web-ui.md#status) - the built Svelte/Vite
    // scaffold plus admin auth. Started after Wi-Fi connects, and after
    // wifi_setup.cpp's own temporary SoftAP-setup server has already
    // stopped, so there's no port/lifecycle overlap between the two.
    // Real settings/diagnostics pages are still future passes. Declared
    // here (not in a narrower scope) so it stays alive for the rest of
    // app_main's life, which never returns.
    homedeck::FirmwareHttpServer web_server;
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
    homedeck::RegisterDiagnosticsRoutes(web_server, storage, admin_auth, []() -> std::optional<std::string> {
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
    if (web_server.Start(80)) {
        printf("Web UI listening on port 80\n");
    } else {
        printf("Web UI failed to start\n");
    }

    uint32_t heartbeat = 0;
    while (true) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
