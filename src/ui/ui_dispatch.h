#pragma once

#include <functional>

namespace homedeck {

// The lv_async_call() hand-off EventBus's UI dispatch routes through -
// see EventBus::SetUiDispatcher. Pure LVGL API, not backend-specific,
// so this same function is what both UiTask (simulator, SDL2) and
// app_main() (firmware) register, and what firmware's own
// non-EventBus UI hand-offs (e.g. wifi_setup.cpp's connection
// callbacks, which fire from a non-UI task - see ADR-0011) call
// directly. Safe to call from any thread; fn runs on whichever thread
// next calls lv_timer_handler().
void PostToUiThread(std::function<void()> fn);

}  // namespace homedeck
