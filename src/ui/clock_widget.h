#pragma once

#include "core/event_bus.h"
#include "lvgl.h"
#include "ui/widget.h"

namespace homedeck {

// Large clock (Core, optional) - see docs/architecture/dashboard.md's
// Android-style pairing of a compact status-bar clock with an optional
// larger clock widget on the home screen. The first real grid widget
// built on DashboardGrid's mixed-span placement mechanism.
class ClockWidget : public Widget {
public:
    explicit ClockWidget(lv_obj_t* parent, EventBus& event_bus);

    lv_obj_t* Root() const override { return root_; }
    int ColumnSpan() const override { return 2; }

private:
    lv_obj_t* root_;
    lv_obj_t* time_label_;
    lv_obj_t* date_label_;
    // Same "reuse Clock's existing tick" reasoning as StatusBar's own
    // clock_subscription_ - see status_bar.h.
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
