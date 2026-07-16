#include <stdio.h>

#include "bsp/m5stack_tab5.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

// Deliberately minimal M1 hardware bring-up: proves the toolchain-to-device
// pipeline (build, flash, boot, serial console) works on the real Tab5.
// Display init uses Espressif's own official m5stack_tab5 BSP component
// (runtime-probes ili9881c+gt911 vs. st7123+st7123), not M5Unified/M5GFX's
// Arduino-as-Component path, which has a confirmed crash on this exact chip
// (m5stack/M5Unified#231) - see
// docs/decisions/ADR-0009-touch-display-detection.md. Replaced once real
// UI bring-up lands - not meant to survive as product code, same as the
// simulator's earlier throwaway heartbeat screen.
void app_main(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("HomeDeck firmware boot\n");
    printf("  IDF version:  %s\n", esp_get_idf_version());
    printf("  Chip:         %s, cores: %d, revision: v%d.%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    printf("  Free heap:    %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Free PSRAM:   %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    printf("Starting display...\n");
    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        printf("bsp_display_start() FAILED - display is NULL\n");
    } else {
        printf("bsp_display_start() OK\n");
        bsp_display_backlight_on();

        bsp_display_lock(0);
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x2060A0), 0);
        bsp_display_unlock();
        printf("Solid color set on active screen\n");
    }

    uint32_t heartbeat = 0;
    while (1) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
