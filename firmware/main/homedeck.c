#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Deliberately minimal M1 hardware bring-up: proves the toolchain-to-device
// pipeline (build, flash, boot, serial console) works on the real Tab5,
// independent of M5Unified/M5GFX - see docs/architecture/hardware.md's
// "Battery-optional operation" section's sibling note on the M5Unified/
// ESP32-P4 Arduino-as-Component crash risk (m5stack/M5Unified#231) this
// step is deliberately kept clear of. Replaced once real display/UI bring-up
// lands - not meant to survive as product code, same as the simulator's
// earlier throwaway heartbeat screen.
void app_main(void) {
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    printf("HomeDeck firmware boot\n");
    printf("  IDF version:  %s\n", esp_get_idf_version());
    printf("  Chip:         %s, cores: %d, revision: v%d.%d\n", CONFIG_IDF_TARGET,
           chip_info.cores, chip_info.revision / 100, chip_info.revision % 100);
    printf("  Free heap:    %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("  Free PSRAM:   %zu bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    uint32_t heartbeat = 0;
    while (1) {
        printf("HomeDeck heartbeat #%lu\n", (unsigned long)heartbeat++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
