#include "lvgl.h"

// Matches the Tab5's confirmed 1280x720 display (see docs/architecture/hardware.md).
static constexpr int32_t kWindowWidth = 1280;
static constexpr int32_t kWindowHeight = 720;

// Proves the CMake + LVGL + SDL2 combination builds and runs, per
// docs/roadmap.md's M1 "first action". No Core, event bus, or real UI yet —
// this is replaced once the dashboard shell work starts.
int main() {
    lv_init();

    lv_display_t* display = lv_sdl_window_create(kWindowWidth, kWindowHeight);
    lv_sdl_mouse_create();

    lv_obj_t* label = lv_label_create(lv_display_get_screen_active(display));
    lv_label_set_text(label, "HomeDeck Simulator");
    lv_obj_center(label);

    while (true) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
}
