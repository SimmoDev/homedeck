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
    lv_scr_load(it->second);
}

void Navigation::GoHome() { GoTo(home_route_); }

}  // namespace homedeck
