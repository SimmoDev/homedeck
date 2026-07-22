#pragma once

#include "core/event_bus.h"
#include "core/notification.h"
#include "lvgl.h"

namespace homedeck {

// A transient overlay shown above whatever screen is currently active -
// see docs/architecture/ui.md#notification-presentation. Parented to
// LVGL's own top layer (lv_layer_top()), not any particular screen's
// root, since it must render regardless of which screen Navigation has
// currently loaded - unlike StatusBar, which is per-screen chrome and
// therefore constructed once per screen. A single NotificationBanner
// instance covers every screen.
class NotificationBanner {
public:
    explicit NotificationBanner(EventBus& event_bus);

private:
    void Show(const std::string& message);

    lv_obj_t* banner_;
    lv_obj_t* label_;
    // A single reused timer, not one created fresh per Show() call - see
    // Show()'s own comment for why.
    lv_timer_t* dismiss_timer_;
    EventBus::ScopedSubscription subscription_;
};

}  // namespace homedeck
