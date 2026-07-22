#include "ui/notification_banner.h"

#include "ui/status_bar.h"

namespace homedeck {

namespace {

constexpr uint32_t kAutoDismissMs = 4000;

void OnAutoDismiss(lv_timer_t* timer) {
    auto* banner = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

NotificationBanner::NotificationBanner(EventBus& event_bus) {
    banner_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(banner_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(banner_, LV_ALIGN_TOP_MID, 0, StatusBar::kHeight);
    lv_obj_set_style_pad_all(banner_, 12, 0);
    lv_obj_set_style_radius(banner_, 0, 0);
    lv_obj_set_style_border_width(banner_, 0, 0);
    // A placeholder accent color, not a considered choice - real
    // notification styling (severity-based color, iconography) is M7
    // Themes scope, same as the rest of the UI's still-default theming
    // (see docs/architecture/dashboard.md#status).
    lv_obj_set_style_bg_color(banner_, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_set_style_bg_opa(banner_, LV_OPA_COVER, 0);
    lv_obj_clear_flag(banner_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);  // nothing to show until the first notification

    label_ = lv_label_create(banner_);
    lv_obj_set_style_text_color(label_, lv_color_white(), 0);
    // Montserrat 24, same as the status bar/dashboard widgets/keyboard
    // (see docs/architecture/dashboard.md#status) - LVGL's own default
    // (Montserrat 14) reads noticeably smaller than everything else on
    // screen without this.
    lv_obj_set_style_text_font(label_, &lv_font_montserrat_24, 0);
    lv_obj_set_width(label_, LV_PCT(100));
    lv_label_set_long_mode(label_, LV_LABEL_LONG_WRAP);

    subscription_ = event_bus.SubscribeUi<NotificationEvent>(
        [this](const NotificationEvent& event) { Show(event.message); });
}

void NotificationBanner::Show(const std::string& message) {
    lv_label_set_text(label_, message.c_str());
    lv_obj_clear_flag(banner_, LV_OBJ_FLAG_HIDDEN);

    // One-shot: fires once after kAutoDismissMs, then deletes itself -
    // see lv_timer_set_auto_delete. A notification arriving while the
    // banner is already visible just restarts the dismiss countdown via
    // a fresh timer instance; the previous one still fires harmlessly
    // against an already-hidden (or re-hidden) banner.
    lv_timer_t* timer = lv_timer_create(OnAutoDismiss, kAutoDismissMs, banner_);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_auto_delete(timer, true);
}

}  // namespace homedeck
