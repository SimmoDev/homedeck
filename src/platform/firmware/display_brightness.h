#pragma once

#include "platform/display_brightness.h"

namespace homedeck {

// Backs DisplayBrightness with the vendored M5Stack Tab5 BSP's
// bsp_display_brightness_set() - PWM-driven, already initialized by
// bsp_display_start() (see bsp_display.c), so no separate init call is
// needed here.
class FirmwareDisplayBrightness : public DisplayBrightness {
public:
    void SetPercent(int percent) override;
};

}  // namespace homedeck
