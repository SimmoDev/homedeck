#include "ui/screens/wifi_setup_screen.h"

namespace homedeck {

namespace {

// Matches StatusBar's own choice (see status_bar.cpp) - the closest
// LVGL-available discrete size to Android's status-bar text on this
// panel's ~294 PPI (see dashboard.md#status for the math). Applied at
// root_ so every label/textarea/button on this screen inherits it rather
// than falling back to LVGL's small 14pt default. The on-screen keyboard
// needs its own separate font setting - see keyboard_input.cpp's own
// comment for why inheritance doesn't reach it.
constexpr const lv_font_t* kFont = &lv_font_montserrat_24;

}  // namespace

WifiSetupScreen::WifiSetupScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  SubmitCallback on_submit)
    : root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      keyboard_(root_),
      on_submit_(std::move(on_submit)) {
    lv_obj_set_style_text_font(root_, kFont, 0);

    lv_obj_t* container = lv_obj_create(root_);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, StatusBar::kHeight + 24);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 12, 0);
    lv_obj_set_style_pad_all(container, 16, 0);

    lv_obj_t* title = lv_label_create(container);
    lv_label_set_text(title, "Connect to Wi-Fi");

    ssid_field_ = lv_textarea_create(container);
    lv_textarea_set_one_line(ssid_field_, true);
    lv_textarea_set_placeholder_text(ssid_field_, "Network name (SSID)");
    lv_obj_set_width(ssid_field_, LV_PCT(90));
    keyboard_.AttachTo(ssid_field_);

    password_field_ = lv_textarea_create(container);
    lv_textarea_set_one_line(password_field_, true);
    lv_textarea_set_password_mode(password_field_, true);
    lv_textarea_set_placeholder_text(password_field_, "Password");
    lv_obj_set_width(password_field_, LV_PCT(90));
    keyboard_.AttachTo(password_field_);

    lv_obj_t* connect_button = lv_button_create(container);
    lv_obj_add_event_cb(connect_button, OnConnectClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, "Connect");

    ap_info_label_ = lv_label_create(container);
    lv_label_set_long_mode(ap_info_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ap_info_label_, LV_PCT(90));
    lv_obj_set_style_text_align(ap_info_label_, LV_TEXT_ALIGN_CENTER, 0);
    // Extra separation on top of container's own pad_row gap, so this
    // reads as a distinct secondary block rather than another field
    // stacked at the same spacing as everything above it.
    lv_obj_set_style_pad_top(ap_info_label_, 24, 0);

    error_label_ = lv_label_create(container);
    lv_label_set_text(error_label_, "");  // LVGL defaults a new label's text to "Text" otherwise.
    lv_label_set_long_mode(error_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(error_label_, LV_PCT(90));
    lv_obj_set_style_text_align(error_label_, LV_TEXT_ALIGN_CENTER, 0);
    // Plain red, not a considered choice - same placeholder-styling
    // rationale as NotificationBanner's own accent color; real severity
    // styling is M7 scope.
    lv_obj_set_style_text_color(error_label_, lv_palette_main(LV_PALETTE_RED), 0);
}

WifiSetupScreen::~WifiSetupScreen() { lv_obj_del(root_); }

void WifiSetupScreen::SetApInfo(const std::string& ap_ssid, const std::string& ap_ip) {
    lv_label_set_text_fmt(ap_info_label_,
                           "To configure Wi-Fi from a computer or phone, connect to SSID \"%s\" and then "
                           "navigate to http://%s/ in a web browser.",
                           ap_ssid.c_str(), ap_ip.c_str());
}

void WifiSetupScreen::SetConnectError(const std::string& message) { lv_label_set_text(error_label_, message.c_str()); }

void WifiSetupScreen::OnConnectClicked(lv_event_t* e) {
    auto* self = static_cast<WifiSetupScreen*>(lv_event_get_user_data(e));
    // A previous attempt's error no longer applies to this one. Empty
    // text is enough to make an LV_LABEL_LONG_WRAP label take no visible
    // space - no need for a second, redundant hidden/visible flag.
    lv_label_set_text(self->error_label_, "");
    if (!self->on_submit_) {
        return;
    }
    bool accepted =
        self->on_submit_(lv_textarea_get_text(self->ssid_field_), lv_textarea_get_text(self->password_field_));
    if (!accepted) {
        self->SetConnectError("Network name (SSID) is required.");
    }
}

}  // namespace homedeck
