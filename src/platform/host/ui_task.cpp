#include "platform/host/ui_task.h"

#include "ui/ui_dispatch.h"

#include <SDL2/SDL.h>

namespace homedeck {

UiTask::UiTask(int32_t width, int32_t height, EventBus& event_bus, float zoom) {
    // Reduces (doesn't eliminate) the text softening a non-1.0 zoom
    // causes - see simulator/README.md. Only visible on a real
    // GPU-accelerated renderer; software renderers (Xvfb, CI) silently
    // ignore anisotropic filtering. Must be set before
    // lv_sdl_window_create() creates the SDL renderer/texture.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
    lv_init();

    // The default group: LVGL auto-adds every subsequently-created
    // focusable widget (buttons, textareas - anything with
    // group_def=true, see lv_obj_class_init_obj()) to whatever group is
    // set as default, with no per-screen wiring needed. Must be set
    // before any screen/widget is constructed - safe here since that
    // only happens after main() finishes constructing this UiTask.
    lv_group_set_default(lv_group_create());

    lv_display_t* display = lv_sdl_window_create(width, height);
    lv_sdl_window_set_title(display, "HomeDeck Simulator");
    lv_sdl_window_set_zoom(display, zoom);
    lv_sdl_mouse_create();
    // Dev-tooling only, no on-device equivalent (touch is the only
    // input on real hardware) - see docs/architecture/simulator.md#status.
    // Routes through the default group above, so Tab/arrow-key/Enter
    // navigation and typing work on any textarea or button without
    // clicking the on-screen keyboard.
    lv_indev_set_group(lv_sdl_keyboard_create(), lv_group_get_default());

    event_bus.SetUiDispatcher(PostToUiThread);
}

void UiTask::Run() {
    while (true) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
}

}  // namespace homedeck
