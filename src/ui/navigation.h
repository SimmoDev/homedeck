#pragma once

#include "lvgl.h"

#include <string>
#include <unordered_map>

namespace homedeck {

// Central screen/route registry - see core.md's Navigation responsibility
// and ui.md's Navigation model. Screens register themselves; they don't
// push/pop each other directly. Lives in ui/, not core/, despite being a
// Core responsibility conceptually - like DashboardScreen, its
// implementation necessarily calls lv_scr_load(), which Core itself must
// never depend on directly.
class Navigation {
public:
    // Registers and immediately loads home_screen - see ui.md's
    // "predictable home screen" invariant. Required, not optional: once
    // screens own their own root objects instead of reusing LVGL's
    // implicit default screen, nothing else makes the dashboard visible
    // at startup.
    Navigation(const std::string& home_route, lv_obj_t* home_screen);

    void Register(const std::string& route, lv_obj_t* screen);
    void GoTo(const std::string& route);
    void GoHome();

private:
    std::string home_route_;
    std::unordered_map<std::string, lv_obj_t*> screens_;
};

}  // namespace homedeck
