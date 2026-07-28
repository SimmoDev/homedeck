#pragma once

#include <functional>

namespace homedeck {

// Creates the recurring LVGL timer PostToUiThread's queue drains
// through - see ui_dispatch.cpp for why a single recurring timer
// replaces lv_async_call()'s one-shot-timer-per-call default. Must be
// called exactly once, from the UI thread, before PostToUiThread can
// usefully run (calls made before this has run just wait in the queue).
// It calls lv_timer_create(), itself an lv_* API call - see ADR-0011 -
// so callers on firmware must wrap this in
// bsp_display_lock()/bsp_display_unlock() (see firmware/main/homedeck.cpp);
// UiTask's constructor needs no such lock, see its own call site.
void InitUiDispatchQueue();

// The hand-off EventBus's UI dispatch routes through - see
// EventBus::SetUiDispatcher. Pure LVGL API, not backend-specific, so
// this same function is what both UiTask (simulator, SDL2) and
// app_main() (firmware) register, and what firmware's own non-EventBus
// UI hand-offs (e.g. wifi_setup.cpp's connection callbacks, which fire
// from a non-UI task - see ADR-0011) call directly. Safe to call from
// any thread; fn runs on the UI thread, in the order PostToUiThread was
// called across all callers (real FIFO - see ui_dispatch.cpp).
void PostToUiThread(std::function<void()> fn);

}  // namespace homedeck
