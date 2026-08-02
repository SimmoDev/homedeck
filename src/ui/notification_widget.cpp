#include "ui/notification_widget.h"

#include "ui/theme.h"
#include "ui/widget_tile.h"

namespace homedeck {

NotificationWidget::NotificationWidget(lv_obj_t* parent, EventBus& event_bus) {
    root_ = CreateWidgetTileRoot(parent);

    title_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(title_label_, kBodyFont, 0);
    lv_label_set_text(title_label_, "Notifications");

    message_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(message_label_, kBodyFont, 0);
    // NotificationEvent::message is an arbitrary string (future modules,
    // e.g. Uptime Kuma, may send long text) - truncate with an ellipsis
    // rather than wrap, same reasoning as NetworkStatusWidget's
    // ssid_label_/WeatherWidget's location_label_: this is a
    // fixed-height tile, and a wrapped second line would overflow it.
    lv_label_set_long_mode(message_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(message_label_, LV_PCT(100));
    lv_obj_set_style_text_align(message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(message_label_, "No notifications yet");

    subscription_ = event_bus.SubscribeUi<NotificationEvent>(
        [this](const NotificationEvent& event) { lv_label_set_text(message_label_, event.message.c_str()); });
}

}  // namespace homedeck
