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

    // Created once, reused for the life of this instance rather than a
    // fresh timer per Show() call - see Show()'s own comment for why.
    // Starts paused: nothing to dismiss until the first notification.
    dismiss_timer_ = lv_timer_create(OnAutoDismiss, kAutoDismissMs, banner_);
    lv_timer_set_repeat_count(dismiss_timer_, 1);
    lv_timer_set_auto_delete(dismiss_timer_, false);
    lv_timer_pause(dismiss_timer_);

    subscription_ = event_bus.SubscribeUi<NotificationEvent>(
        [this](const NotificationEvent& event) { Show(event.message); });
}

void NotificationBanner::Show(const std::string& message) {
    lv_label_set_text(label_, message.c_str());
    lv_obj_clear_flag(banner_, LV_OBJ_FLAG_HIDDEN);
    // A notification must never end up silently hidden behind whatever
    // else is on lv_layer_top() at the time (e.g. the quick-settings
    // panel) - move_foreground guarantees this regardless of what else
    // has been added to the layer, rather than depending on every other
    // top-layer overlay knowing to stay behind this one.
    lv_obj_move_foreground(banner_);

    // Reusing the same timer (reset + resume + re-arm its repeat count,
    // rather than creating a new one-shot timer per call) means a
    // notification arriving while the banner is already visible always
    // restarts the same kAutoDismissMs countdown from now - a fresh
    // timer per call would leave the *earlier* call's timer still
    // pending, which could fire first and hide the banner before this
    // notification's own window elapses.
    lv_timer_set_repeat_count(dismiss_timer_, 1);
    lv_timer_reset(dismiss_timer_);
    lv_timer_resume(dismiss_timer_);
}

}  // namespace homedeck
