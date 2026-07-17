#include <cstdio>

#include "bsp/m5stack_tab5.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/firmware/battery_reader.h"
#include "platform/firmware/time_source.h"
#include "ui/screens/dashboard_screen.h"

// --- M2 Wi-Fi/ESP-Hosted bring-up (throwaway, see docs/roadmap.md's M2
// Wi-Fi item) ---
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if __has_include("wifi_credentials.local.h")
#include "wifi_credentials.local.h"
#else
#error \
    "Copy firmware/main/wifi_credentials.local.h.example to \
wifi_credentials.local.h and fill in a real network - never commit the \
real file (see .gitignore)."
#endif

// The real HomeDeck dashboard, running on-device - see docs/roadmap.md's
// M1 "Basic LVGL application running on-device" item. Display and touch
// are confirmed working on this hardware - see
// docs/architecture/hardware.md#display-driver-strategy.
//
// Deliberately out of scope here: Navigation, the home affordance, and
// any second screen - the dashboard is loaded directly as the only
// screen, matching this step's scope in docs/roadmap.md. No
// firmware/platform/ Task/Queue/Timer backend existed before this
// change; task.cpp and timer.cpp (FreeRTOS-backed, per ADR-0002) are
// added alongside this specifically because Clock needs a working Timer.
namespace {

// Mirrors the exact lv_async_call()-based hand-off UiTask uses for the
// simulator (src/ui/ui_task.cpp) - this is core LVGL API, not backend-
// specific, so the same mechanism applies whether LVGL is being driven
// by SDL2 or (as here) espressif/m5stack_tab5's BSP.
void RunAndDelete(void* user_data) {
    auto* fn = static_cast<std::function<void()>*>(user_data);
    (*fn)();
    delete fn;
}

// Throwaway M2 bring-up: proves Wi-Fi actually connects and gets an IP
// over the ESP32-C6/SDIO link (see hardware.md#wireless) before the real
// SoftAP + wifi_provisioning flow (ADR-0006) is built on top of it.
// Deleted once that flow exists - real credentials belong in Core's
// encrypted NVS storage (ADR-0010), never compiled into firmware, which
// is exactly why this reads from a gitignored local header instead.
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;
constexpr int kMaxConnectAttempts = 5;

void OnWifiOrIpEvent(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int attempt = 0;
    auto* wifi_event_group = static_cast<EventGroupHandle_t>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (attempt < kMaxConnectAttempts) {
            attempt++;
            printf("Wi-Fi bring-up: disconnected, retry %d/%d\n", attempt, kMaxConnectAttempts);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, kWifiFailedBit);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        printf("Wi-Fi bring-up: connected, IP=" IPSTR "\n", IP2STR(&event->ip_info.ip));
        attempt = 0;
        xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
    }
}

// Blocks up to ~10s proving real connectivity, then always continues into
// the rest of app_main() regardless of outcome - the dashboard must still
// boot offline, per CLAUDE.md's offline-degradation requirement.
void RunWifiBringupTest() {
    // The C6 co-processor's power rail (WLAN_PWR_EN) isn't on by default -
    // it's bit 0 of a dedicated I2C GPIO expander output (see
    // hardware.md#wireless). bsp_feature_enable() brings up the shared I2C
    // bus itself if it isn't already (same pattern bsp_touch_new() uses),
    // so this is safe to call this early, before bsp_display_start(). Real
    // Wi-Fi bring-up needed this and the correct SDIO pin config below
    // together, confirmed working as a pair, not confirmed independently -
    // dropping this call isn't verified safe on its own.
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    EventGroupHandle_t wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnWifiOrIpEvent,
                                                wifi_event_group));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnWifiOrIpEvent,
                                                wifi_event_group));

    wifi_config_t wifi_config = {};
    std::snprintf(reinterpret_cast<char*>(wifi_config.sta.ssid), sizeof(wifi_config.sta.ssid), "%s",
                  WIFI_BRINGUP_SSID);
    std::snprintf(reinterpret_cast<char*>(wifi_config.sta.password), sizeof(wifi_config.sta.password),
                  "%s", WIFI_BRINGUP_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("Wi-Fi bring-up: connecting to \"%s\"...\n", WIFI_BRINGUP_SSID);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit | kWifiFailedBit, pdTRUE,
                                            pdFALSE, pdMS_TO_TICKS(10000));
    if (bits & kWifiConnectedBit) {
        printf("Wi-Fi bring-up: SUCCESS\n");
    } else if (bits & kWifiFailedBit) {
        printf("Wi-Fi bring-up: FAILED after %d attempts\n", kMaxConnectAttempts);
    } else {
        printf("Wi-Fi bring-up: TIMED OUT waiting for connection\n");
    }
}

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

    RunWifiBringupTest();

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
    lv_scr_load(dashboard.Root());
    bsp_display_unlock();
    printf("Dashboard loaded\n");

    // Clock (the ClockTickEvent publisher) must exist after the
    // dashboard (the subscriber) is already listening - see
    // simulator/main.cpp's identical ordering note. Clock publishes one
    // tick immediately at construction so the display doesn't sit on
    // LVGL's placeholder text until the first periodic tick.
    homedeck::Clock clock(time_source, event_bus);

    uint32_t heartbeat = 0;
    while (true) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
