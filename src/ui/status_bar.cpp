#include "ui/status_bar.h"

#include "core/low_battery_monitor.h"
#include "ui/quick_settings_panel.h"
#include "ui/theme.h"
#include "ui/time_format.h"

#include <cstdio>

namespace homedeck {

namespace {

void OnSettingsIconClicked(lv_event_t* e) {
    auto* event_bus = static_cast<EventBus*>(lv_event_get_user_data(e));
    event_bus->Publish(QuickSettingsRequestedEvent{});
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
    bar_ = lv_obj_create(parent);
    lv_obj_set_size(bar_, LV_PCT(100), kHeight);
    lv_obj_align(bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(bar_, 0, 0);
    lv_obj_set_style_radius(bar_, 0, 0);
    lv_obj_set_style_border_width(bar_, 0, 0);
    // Solid dark chrome, closer to how Android/iOS render a status bar,
    // rather than blending into the light default theme background.
    lv_obj_set_style_bg_color(bar_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    // lv_obj_create() is scrollable by default (for arbitrary content
    // containers); a fixed-height status bar should never itself scroll -
    // leaving it enabled produces a visible scrollbar and drag-to-scroll
    // on what's meant to be static chrome.
    lv_obj_clear_flag(bar_, LV_OBJ_FLAG_SCROLLABLE);
    // Without this, `bar_` is just a normal child of `parent` (each
    // screen's own scrolling root_) - it scrolls out of view with the
    // rest of the content on a screen tall enough to scroll, and drags
    // down with root_'s own elastic overscroll bounce on any screen.
    // FLOATING keeps it pinned at its aligned position regardless of
    // root_'s scroll offset, the standard LVGL idiom for a fixed status
    // bar over scrollable content.
    lv_obj_add_flag(bar_, LV_OBJ_FLAG_FLOATING);

    clock_label_ = lv_label_create(bar_);
    lv_obj_set_style_text_font(clock_label_, kBodyFont, 0);
    lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
    lv_obj_align(clock_label_, LV_ALIGN_LEFT_MID, 12, 0);
    // lv_label_create() defaults to showing "Text" until this is set to
    // something real - blank rather than a fabricated time, since
    // there's no real value to show until the first ClockTickEvent
    // arrives (up to one Clock period later). Matches battery_label_'s
    // own immediate RefreshStatusLabel() call below.
    lv_label_set_text(clock_label_, "");

    // A flex row, not two independently-positioned objects - battery_label_'s
    // width varies with its content (Wi-Fi icon, charge icon, percent
    // digits all come and go), and a fixed offset for settings_icon
    // relative to the bar's edge would drift out of sync with it (and
    // lv_obj_align_to() against battery_label_ directly wouldn't help -
    // it computes a one-time position, not a live constraint that tracks
    // later width changes). Flex re-flows both automatically whenever
    // RefreshStatusLabel changes battery_label_'s content, keeping a
    // consistent gap between them and the cluster's right edge pinned via
    // the same lv_obj_align() the rest of this bar already uses.
    lv_obj_t* right_cluster = lv_obj_create(bar_);
    lv_obj_set_size(right_cluster, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_cluster, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_cluster, 0, 0);
    lv_obj_set_style_pad_all(right_cluster, 0, 0);
    lv_obj_set_style_pad_column(right_cluster, 8, 0);
    lv_obj_clear_flag(right_cluster, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right_cluster, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_cluster, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(right_cluster, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_t* settings_icon = lv_label_create(right_cluster);
    lv_obj_set_style_text_font(settings_icon, kBodyFont, 0);
    lv_obj_set_style_text_color(settings_icon, lv_color_white(), 0);
    lv_label_set_text(settings_icon, LV_SYMBOL_SETTINGS);
    lv_obj_add_flag(settings_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_icon, OnSettingsIconClicked, LV_EVENT_CLICKED, &event_bus);

    battery_label_ = lv_label_create(right_cluster);
    lv_obj_set_style_text_font(battery_label_, kBodyFont, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    RefreshStatusLabel(battery_label_, battery_reader_, wifi_connected_);

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
