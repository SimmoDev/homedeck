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
    UiTask(int32_t width, int32_t height, EventBus& event_bus);

    // Runs lv_timer_handler() in a loop. Blocks forever - there is no
    // shutdown path yet, matching there being no exit screen either.
    [[noreturn]] void Run();
};

}  // namespace homedeck
