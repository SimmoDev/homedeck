#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "lvgl.h"
#include "platform/battery_reader.h"

namespace homedeck {

// The persistent home screen - see ADR-0004's dashboard-as-home
// decision. Hosts Core-only widgets directly for now (clock/date,
// battery); the general pluggable widget interface modules will use is
// M2 scope, not this - see docs/architecture/dashboard.md.
class DashboardScreen {
public:
    DashboardScreen(EventBus& event_bus, BatteryReader& battery_reader);

    // Owns its own root (rather than taking an externally-provided
    // parent) so Navigation can lv_scr_load() it directly and switch to
    // other screens that own theirs.
    lv_obj_t* Root() const { return root_; }

private:
    lv_obj_t* root_;
    lv_obj_t* clock_label_;
    lv_obj_t* battery_label_;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
