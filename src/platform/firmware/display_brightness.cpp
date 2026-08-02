#include "platform/firmware/display_brightness.h"

#include "bsp/m5stack_tab5.h"
#include "esp_log.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "display_brightness";

}  // namespace

void FirmwareDisplayBrightness::SetPercent(int percent) {
    int clamped = ClampBrightnessPercent(percent);
    esp_err_t result = bsp_display_brightness_set(clamped);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "bsp_display_brightness_set(%d) failed: %s", clamped, esp_err_to_name(result));
    }
}

}  // namespace homedeck
