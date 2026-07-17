#include "ui/ui_task.h"

#include <SDL2/SDL.h>

#include <functional>
#include <utility>

namespace homedeck {

namespace {

// The lv_async_call() hand-off EventBus's UI dispatch routes through.
// Wraps the std::function in a heap allocation matching lv_async_call's
// single void* user_data slot; freed here once invoked.
void RunAndDelete(void* user_data) {
    auto* fn = static_cast<std::function<void()>*>(user_data);
    (*fn)();
    delete fn;
}

}  // namespace

UiTask::UiTask(int32_t width, int32_t height, EventBus& event_bus, float zoom) {
    // Reduces (doesn't eliminate) the text softening a non-1.0 zoom
    // causes - see simulator/README.md. Only visible on a real
    // GPU-accelerated renderer; software renderers (Xvfb, CI) silently
    // ignore anisotropic filtering. Must be set before
    // lv_sdl_window_create() creates the SDL renderer/texture.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    lv_init();
    lv_display_t* display = lv_sdl_window_create(width, height);
    lv_sdl_window_set_title(display, "HomeDeck Simulator");
    lv_sdl_window_set_zoom(display, zoom);
    lv_sdl_mouse_create();

    event_bus.SetUiDispatcher([](std::function<void()> fn) {
        auto* heap_fn = new std::function<void()>(std::move(fn));
        lv_async_call(RunAndDelete, heap_fn);
    });
}

void UiTask::Run() {
    while (true) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
}

}  // namespace homedeck
