#pragma once

#include "lvgl.h"
#include "ui/navigation.h"

namespace homedeck {

// A persistent on-screen icon that returns to the dashboard - see
// ADR-0004's decision to use a fixed on-screen affordance over a gesture
// or power-button long-press. Every non-dashboard screen includes this in
// its own layout; the dashboard itself doesn't (see ui.md's Navigation
// model - "present on every screen except the dashboard itself"). Returns
// the button so a caller can raise it above content added afterward (see
// StatusBar::Root()'s own comment - same reason, same pattern).
lv_obj_t* CreateHomeAffordance(lv_obj_t* parent, Navigation& navigation);

}  // namespace homedeck
