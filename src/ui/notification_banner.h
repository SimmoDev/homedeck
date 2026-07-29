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
    // banner_ has no owning parent (see ui.md#object-lifecycle) - it's a
    // direct child of lv_layer_top(), which is never itself destroyed.
    // dismiss_timer_ must go first: it's a separate LVGL resource from
    // the object tree and its callback reads banner_, so deleting banner_
    // first would leave it pointing at freed memory until it next fires.
    ~NotificationBanner();

    NotificationBanner(const NotificationBanner&) = delete;
    NotificationBanner& operator=(const NotificationBanner&) = delete;

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
