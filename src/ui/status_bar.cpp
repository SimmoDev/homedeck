#include "ui/status_bar.h"

#include <cstdio>
#include <ctime>

namespace homedeck {

namespace {

constexpr int32_t kBarHeight = 48;

// localtime_r is POSIX; matches Linux being the only currently-verified
// simulator platform (see DEVELOPMENT.md) - same caveat as
// DashboardScreen's now-removed clock formatting had.
void FormatCompactTime(std::chrono::system_clock::time_point time, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, "%H:%M  %a %d %b", &tm);
}

void RefreshBatteryLabel(lv_obj_t* label, BatteryReader& battery_reader) {
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", battery_reader.ReadPercent());
    lv_label_set_text(label, text);
}

}  // namespace

StatusBar::StatusBar(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader)
    : battery_reader_(battery_reader) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), kBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    // Solid dark chrome, closer to how Android/iOS render a status bar,
    // rather than blending into the light default theme background.
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // lv_obj_create() is scrollable by default (for arbitrary content
    // containers); a fixed-height status bar should never itself scroll
    // - confirmed on hardware that leaving this on produces a visible
    // scrollbar and drag-to-scroll behavior on what's meant to be static
    // chrome.
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    clock_label_ = lv_label_create(bar);
    lv_obj_set_style_text_font(clock_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
    lv_obj_align(clock_label_, LV_ALIGN_LEFT_MID, 12, 0);

    battery_label_ = lv_label_create(bar);
    lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    RefreshBatteryLabel(battery_label_, battery_reader_);
    lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -12, 0);

    clock_subscription_ =
        event_bus.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent& event) {
            char text[32];
            FormatCompactTime(event.time, text, sizeof(text));
            lv_label_set_text(clock_label_, text);

            RefreshBatteryLabel(battery_label_, battery_reader_);
        });
}

}  // namespace homedeck
