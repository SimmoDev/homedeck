#include "ui/navigation.h"

namespace homedeck {

Navigation::Navigation(const std::string& home_route, lv_obj_t* home_screen)
    : home_route_(home_route) {
    Register(home_route, home_screen);
    lv_scr_load(home_screen);
}

void Navigation::Register(const std::string& route, lv_obj_t* screen) {
    screens_[route] = screen;
}

void Navigation::GoTo(const std::string& route) {
    auto it = screens_.find(route);
    if (it == screens_.end()) return;
    // A caller doesn't generally know (or need to know) whether it's
    // already on the target screen - e.g. ConnectToWifi()'s on_connected
    // callback calls GoHome() unconditionally once Wi-Fi actually
    // associates, whether or not the dashboard was already active (the
    // common case, since stored credentials load it immediately).
    // Reloading an already-active screen is a real, wasted cost even
    // though it's a no-visible-difference redraw: lv_scr_load() forces a
    // full-screen redraw regardless of whether anything on screen
    // actually changed.
    if (it->second == lv_screen_active()) return;
    lv_scr_load(it->second);
}

void Navigation::GoHome() { GoTo(home_route_); }

}  // namespace homedeck
