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
    DashboardScreen(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader);

private:
    lv_obj_t* clock_label_;
    lv_obj_t* battery_label_;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
