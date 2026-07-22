#include "ui/status_bar.h"

#include "core/low_battery_monitor.h"
#include "ui/time_format.h"

#include <cstdio>

namespace homedeck {

namespace {

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
//   - Battery, not actively charging: level icon + percent - the common
//     discharging case, and also what a plugged-in battery that's
//     stopped drawing charge current shows (topped off, or between the
//     charger's own recharge-threshold cycles) - these two are
//     indistinguishable with the signals available on this board (see
//     hardware.md#power's Charge status bullet), and showing a charge
//     symbol here would be misleading regardless of which one it is.
//   - Battery, actively charging (percent < 100 as a UI guard against
//     flashing "charging" right at the 100% boundary): charge symbol +
//     percent, not the level icon - LVGL's symbol font has no single
//     glyph combining "charging" with a specific fill level, and the
//     charge symbol alone is the clearer signal while it's actively
//     rising.
//   - No battery (always with external power - nothing would be
//     running otherwise): USB symbol alone, not the charge symbol -
//     there's no battery to charge, this is just "running on USB
//     power." ReadPercent() is not meaningful without a battery to
//     report a percentage of (its underlying voltage reading swings
//     unpredictably with nothing connected to charge - see
//     Ina226BatteryReader::IsBatteryPresent()), so it's never shown in
//     this state, not even a stale/misleading number.
// Wi-Fi icon and battery status share one label so LVGL lays out their
// spacing uniformly - two spaces between every symbol, matching the
// existing "%s  %d%%" convention below, rather than two independently-
// positioned objects with a guessed pixel offset between them.
void RefreshStatusLabel(lv_obj_t* label, BatteryReader& battery_reader, bool wifi_connected) {
    // 32, not 16: GCC's format-truncation check at -O2 sizes against
    // %d's full int range, not ReadPercent()'s real 0-100 - 16 is
    // provably enough for any real value but not for the type's range.
    char text[32];
    bool battery_present = battery_reader.IsBatteryPresent();
    int percent = battery_reader.ReadPercent();
    bool charging = battery_present && battery_reader.IsExternalPowerConnected() && percent < 100;
    const char* wifi_prefix = wifi_connected ? LV_SYMBOL_WIFI "  " : "";
    if (!battery_present) {
        std::snprintf(text, sizeof(text), "%s" LV_SYMBOL_USB, wifi_prefix);
    } else if (charging) {
        std::snprintf(text, sizeof(text), "%s" LV_SYMBOL_CHARGE "  %d%%", wifi_prefix, percent);
    } else {
        std::snprintf(text, sizeof(text), "%s%s  %d%%", wifi_prefix, BatteryLevelIcon(percent), percent);
    }
    lv_label_set_text(label, text);
}

}  // namespace

StatusBar::StatusBar(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status)
    : battery_reader_(battery_reader), wifi_connected_(network_status.Snapshot().connected) {
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
    // lv_label_create() defaults to showing "Text" until this is set to
    // something real - blank rather than a fabricated time, since
    // there's no real value to show until the first ClockTickEvent
    // arrives (up to one Clock period later). Matches battery_label_'s
    // own immediate RefreshStatusLabel() call below.
    lv_label_set_text(clock_label_, "");

    battery_label_ = lv_label_create(bar);
    lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    RefreshStatusLabel(battery_label_, battery_reader_, wifi_connected_);
    lv_obj_align(battery_label_, LV_ALIGN_RIGHT_MID, -12, 0);

    clock_subscription_ =
        event_bus.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent& event) {
            char text[32];
            FormatLocalTime(event.time, "%H:%M  %a %d %b", text, sizeof(text));
            lv_label_set_text(clock_label_, text);

            RefreshStatusLabel(battery_label_, battery_reader_, wifi_connected_);
        });

    wifi_subscription_ =
        event_bus.SubscribeUi<WifiConnectivityChangedEvent>([this](const WifiConnectivityChangedEvent& event) {
            wifi_connected_ = event.connected;
            RefreshStatusLabel(battery_label_, battery_reader_, wifi_connected_);
        });
}

}  // namespace homedeck
