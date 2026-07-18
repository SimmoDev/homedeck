#pragma once

#include "core/event_bus.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "ui/status_bar.h"

namespace homedeck {

// The persistent home screen - see ADR-0004's dashboard-as-home
// decision. Compact date/time and battery live in the shared StatusBar
// chrome (see ADR-0008's "Status bar vs. dashboard-only widgets"
// decision), not as dashboard-specific labels. The general pluggable
// widget interface modules will use is separate, still-unbuilt M2 scope
// - see docs/architecture/dashboard.md.
class DashboardScreen {
public:
    DashboardScreen(EventBus& event_bus, BatteryReader& battery_reader);

    // Owns its own root (rather than taking an externally-provided
    // parent) so Navigation can lv_scr_load() it directly and switch to
    // other screens that own theirs.
    lv_obj_t* Root() const { return root_; }

private:
    lv_obj_t* root_;
    StatusBar status_bar_;
};

}  // namespace homedeck
