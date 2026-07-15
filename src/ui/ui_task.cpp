#include "ui/ui_task.h"

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

UiTask::UiTask(int32_t width, int32_t height, EventBus& event_bus) {
    lv_init();
    display_ = lv_sdl_window_create(width, height);
    lv_sdl_window_set_title(display_, "HomeDeck Simulator");
    lv_sdl_mouse_create();

    event_bus.SetUiDispatcher([](std::function<void()> fn) {
        auto* heap_fn = new std::function<void()>(std::move(fn));
        lv_async_call(RunAndDelete, heap_fn);
    });
}

lv_obj_t* UiTask::ActiveScreen() const { return lv_display_get_screen_active(display_); }

void UiTask::Run() {
    while (true) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
}

}  // namespace homedeck
