#include "ui/screen_loader.h"

#include <utility>

namespace homedeck {

ScreenLoader::ScreenLoader(lv_obj_t* screen, ConnectedFn connected, LoadFn load)
    : screen_(screen), connected_(std::move(connected)), load_(std::move(load)) {
    // The screen owns the registration: deleting it removes the callback.
    lv_obj_add_event_cb(screen_, OnScreenLoadStart, LV_EVENT_SCREEN_LOAD_START, this);
}

void ScreenLoader::OnConnected() {
    if (lazy_load_.OnConnected(lv_screen_active() == screen_)) {
        load_();
    }
}

void ScreenLoader::OnScreenLoadStart(lv_event_t* e) {
    auto* self = static_cast<ScreenLoader*>(lv_event_get_user_data(e));
    if (self->lazy_load_.OnShown(self->connected_())) {
        self->load_();
    }
}

}  // namespace homedeck
