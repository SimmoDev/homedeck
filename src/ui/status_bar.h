#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "lvgl.h"
#include "platform/battery_reader.h"

namespace homedeck {

// Persistent top status bar - compact date/time and battery, shown on
// every screen including the dashboard (see ADR-0008's "Status bar vs.
// dashboard-only widgets" decision: this is fixed system chrome, not a
// customizable/removable dashboard widget). Each screen creates its own
// StatusBar instance as part of its own layout, the same "each screen
// includes it" pattern home_affordance.h uses - except present on the
// dashboard too, unlike the home affordance, which specifically excludes
// it (see ui.md's Navigation model).
class StatusBar {
public:
    // Exposed so other screen chrome (e.g. DashboardGrid) can position
    // itself below the bar without duplicating this as a magic number.
    static constexpr int32_t kHeight = 48;

    StatusBar(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader);

private:
    BatteryReader& battery_reader_;
    lv_obj_t* clock_label_;
    lv_obj_t* battery_label_;
    // Battery refreshes on Clock's existing tick rather than a second
    // timer - a 1Hz read is more than enough for a percentage display,
    // and reusing the tick already flowing through here matches
    // CLAUDE.md's "use shared scheduling mechanisms where practical" for
    // background work.
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
