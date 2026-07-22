#pragma once

#include "core/event_bus.h"
#include "core/notification.h"
#include "lvgl.h"
#include "ui/widget.h"

namespace homedeck {

// The dashboard-indicator presentation for Core's notification service
// - see docs/architecture/ui.md#notification-presentation. A
// last-notification tile, not an unread-count badge: always echoes the
// most recent NotificationEvent's message, with no seen/unseen state
// to track or clear - see docs/roadmap.md's M7 Polish list for why the
// count variant is deliberately deferred rather than built here.
// Structural twin of NetworkStatusWidget, but with no extra service
// dependency beyond EventBus - unlike WeatherWidget/NetworkStatusWidget,
// there's no other consumer of "last notification" state, so no new
// Core class backs this; it subscribes directly, the same as
// NotificationBanner (ui/notification_banner.h) does for its own
// screen-banner presentation of the same event.
class NotificationWidget : public Widget {
public:
    explicit NotificationWidget(lv_obj_t* parent, EventBus& event_bus);

    lv_obj_t* Root() const override { return root_; }
    // 2 columns, same as the other real tiles.
    int ColumnSpan() const override { return 2; }

private:
    lv_obj_t* root_;
    lv_obj_t* title_label_;
    lv_obj_t* message_label_;
    EventBus::ScopedSubscription subscription_;
};

}  // namespace homedeck
