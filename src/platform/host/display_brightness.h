#pragma once

#include "platform/display_brightness.h"

#include "lvgl.h"

namespace homedeck {

// Backs DisplayBrightness with an LVGL top-layer overlay - not built by
// homedeck_platform_host (see src/CMakeLists.txt's own comment at the
// call site): that library is shared with tests/, which never fetches
// LVGL/SDL2. This is compiled into homedeck_ui instead, the same
// if(TARGET lvgl)-gated library ui_task.cpp/audio_output.cpp already
// live in - there's no real backlight to dim on a desktop SDL2 window,
// so "dimming" here means darkening everything already on screen via a
// semi-transparent overlay rather than a hardware PWM call.
class HostDisplayBrightness : public DisplayBrightness {
public:
    HostDisplayBrightness();

    void SetPercent(int percent) override;

private:
    lv_obj_t* overlay_;
};

}  // namespace homedeck
