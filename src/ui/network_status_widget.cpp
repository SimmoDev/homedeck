#include "ui/network_status_widget.h"

#include "ui/theme.h"
#include "ui/widget_tile.h"

namespace homedeck {

namespace {

void RefreshNetworkLabels(lv_obj_t* state_label, lv_obj_t* ssid_label, lv_obj_t* ip_label,
                           const NetworkState& state) {
    if (state.connected) {
        lv_label_set_text(state_label, LV_SYMBOL_WIFI "  Connected");
        lv_label_set_text(ssid_label, state.ssid.c_str());
        lv_label_set_text(ip_label, state.ip_address.c_str());
        lv_obj_clear_flag(ssid_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ip_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        // SSID/IP hidden rather than blanked - an empty flex child still
        // reserves its line height, leaving dead space under a lone
        // "Disconnected" line instead of centering it in the tile.
        lv_label_set_text(state_label, LV_SYMBOL_WIFI "  Disconnected");
        lv_obj_add_flag(ssid_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ip_label, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

NetworkStatusWidget::NetworkStatusWidget(lv_obj_t* parent, EventBus& event_bus, NetworkStatus& network_status) {
    // Stacks the three lines, centered - see CreateWidgetTileRoot() for
    // the shared tile layout every widget builds on.
    root_ = CreateWidgetTileRoot(parent);

    state_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(state_label_, kBodyFont, 0);

    ssid_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(ssid_label_, kBodyFont, 0);
    // A 32-char SSID (the real max) still won't fit this tile's width at
    // Montserrat 24 - truncate with an ellipsis rather than overflowing
    // into neighboring cells. Elsewhere in this codebase, bounded-width
    // text wraps instead (wifi_setup_screen.cpp, notification_banner.cpp)
    // - wrapping doesn't fit here, since a second SSID line would push
    // the IP label out of this tile's fixed height.
    lv_label_set_long_mode(ssid_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssid_label_, LV_PCT(100));
    lv_obj_set_style_text_align(ssid_label_, LV_TEXT_ALIGN_CENTER, 0);

    ip_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(ip_label_, kBodyFont, 0);

    RefreshNetworkLabels(state_label_, ssid_label_, ip_label_, network_status.Snapshot());

    wifi_subscription_ =
        event_bus.SubscribeUi<WifiConnectivityChangedEvent>([this, &network_status](const WifiConnectivityChangedEvent&) {
            RefreshNetworkLabels(state_label_, ssid_label_, ip_label_, network_status.Snapshot());
        });
}

}  // namespace homedeck
