#pragma once

#include "core/event_bus.h"
#include "lvgl.h"

#include <cstdint>

namespace homedeck {

// Owns LVGL exclusively: the SDL2 window, the lv_timer_handler() loop,
// and the lv_async_call() hand-off for EventBus's UI-facing subscribers
// - see ADR-0011. No other code should call an lv_* function directly.
class UiTask {
public:
    // width/height are LVGL's logical render resolution - kept equal to
    // the real panel's so simulator layout testing matches hardware
    // exactly. zoom scales only the on-screen SDL window size, purely a
    // desktop dev-convenience knob with no on-device equivalent (a
    // physical panel has no "zoom").
    UiTask(int32_t width, int32_t height, EventBus& event_bus, float zoom = 1.0f);

    // Runs lv_timer_handler() in a loop. Blocks forever - there is no
    // shutdown path yet, matching there being no exit screen either.
    [[noreturn]] void Run();
};

}  // namespace homedeck
