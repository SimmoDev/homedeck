#include "ui/status_bar.h"

#include "core/low_battery_monitor.h"

#include <cstdio>
#include <ctime>

namespace homedeck {

namespace {

// localtime_r is POSIX; matches Linux being the only currently-verified
// simulator platform (see DEVELOPMENT.md).
void FormatCompactTime(std::chrono::system_clock::time_point time, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, "%H:%M  %a %d %b", &tm);
}

// Breakpoints for the four non-empty levels are even round numbers, not
// tied to anything else. The empty breakpoint isn't independent - it
// reuses LowBatteryMonitor::kThresholdPercent so the icon reads "empty"
// at exactly the same point the low-battery notification fires, rather
// than two separate magic numbers drifting apart.
const char* BatteryLevelIcon(int percent) {
    if (percent < LowBatteryMonitor::kThresholdPercent) return LV_SYMBOL_BATTERY_EMPTY;
    if (percent < 40) return LV_SYMBOL_BATTERY_1;
    if (percent < 65) return LV_SYMBOL_BATTERY_2;
    if (percent < 90) return LV_SYMBOL_BATTERY_3;
    return LV_SYMBOL_BATTERY_FULL;
}

// See docs/architecture/hardware.md#power for how
// IsBatteryPresent()/IsExternalPowerConnected() are derived.
//   - Battery, no external power: level icon + percent - the common
//     case.
//   - Battery, external power, still charging (percent < 100): charge
//     symbol + percent, not the level icon - LVGL's symbol font has no
//     single glyph combining "charging" with a specific fill level, and
//     the charge symbol alone is the clearer signal while it's actively
//     rising.
//   - Battery, external power, percent == 100: level icon (always
//     full) + percent, same as the no-external-power case - full, so
//     nothing's actually being charged; showing the charge symbol here
//     would be misleading.
//   - No battery (always with external power - nothing would be
//     running otherwise): USB symbol alone, not the charge symbol -
//     there's no battery to charge, this is just "running on USB
//     power." ReadPercent() is not meaningful without a battery to
//     report a percentage of (its underlying voltage reading swings
//     unpredictably with nothing connected to charge - see
//     Ina226BatteryReader::IsBatteryPresent()), so it's never shown in
//     this state, not even a stale/misleading number.
void RefreshBatteryLabel(lv_obj_t* label, BatteryReader& battery_reader) {
    char text[16];
    bool battery_present = battery_reader.IsBatteryPresent();
    int percent = battery_reader.ReadPercent();
    bool charging = battery_present && battery_reader.IsExternalPowerConnected() && percent < 100;
    if (!battery_present) {
        std::snprintf(text, sizeof(text), LV_SYMBOL_USB);
    } else if (charging) {
        std::snprintf(text, sizeof(text), LV_SYMBOL_CHARGE "  %d%%", percent);
    } else {
        std::snprintf(text, sizeof(text), "%s  %d%%", BatteryLevelIcon(percent), percent);
    }
    lv_label_set_text(label, text);
}

}  // namespace

StatusBar::StatusBar(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader)
    : battery_reader_(battery_reader) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), kHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    // Solid dark chrome, closer to how Android/iOS render a status bar,
    // rather than blending into the light default theme background.
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // lv_obj_create() is scrollable by default (for arbitrary content
    // containers); a fixed-height status bar should never itself scroll -
    // leaving it enabled produces a visible scrollbar and drag-to-scroll
    // on what's meant to be static chrome.
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
