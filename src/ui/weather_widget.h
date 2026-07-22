#pragma once

#include "core/event_bus.h"
#include "core/weather_provider.h"
#include "lvgl.h"
#include "ui/widget.h"

namespace homedeck {

// The weather dashboard widget - see
// docs/architecture/dashboard.md's Weather source section and ADR-0008.
// Structural twin of NetworkStatusWidget: refreshes on
// WeatherUpdatedEvent, not the clock tick, since WeatherProvider's own
// poll cadence (~30 min) already governs how often anything can
// meaningfully change here.
class WeatherWidget : public Widget {
public:
    explicit WeatherWidget(lv_obj_t* parent, EventBus& event_bus, WeatherProvider& weather_provider);

    lv_obj_t* Root() const override { return root_; }
    // 2 columns, same reasoning as NetworkStatusWidget - a display name
    // plus temperature and condition text need more room than a 1x1
    // square gives.
    int ColumnSpan() const override { return 2; }

private:
    lv_obj_t* root_;
    lv_obj_t* condition_label_;
    lv_obj_t* location_label_;
    EventBus::ScopedSubscription weather_subscription_;
};

}  // namespace homedeck
