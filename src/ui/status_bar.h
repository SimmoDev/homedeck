#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "core/network_status_monitor.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"

namespace homedeck {

// Persistent top status bar - compact date/time and battery, shown on
// every screen including the dashboard (see ADR-0008's "Status bar vs.
// dashboard-only widgets" decision: this is fixed system chrome, not a
// customizable/removable dashboard widget). Each screen creates its own
// StatusBar instance as part of its own layout, the same "each screen
// includes it" pattern home_affordance.h uses - except present on the
// dashboard too, unlike the home affordance, which specifically excludes
// it (see ui.md's Navigation model).
class StatusBar {
public:
    // Exposed so other screen chrome (e.g. DashboardGrid) can position
    // itself below the bar without duplicating this as a magic number.
    static constexpr int32_t kHeight = 48;

    StatusBar(lv_obj_t* parent, EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status);

    // So a screen can raise the bar above content added after it (see
    // FLOATING's own comment in the constructor - FLOATING only excludes
    // it from scrolling, not from paint order, and status_bar_ is
    // typically a member constructed before the rest of a screen's own
    // content in its constructor body). Call once, after a screen's
    // constructor has added everything else.
    lv_obj_t* Root() const { return bar_; }

private:
    lv_obj_t* bar_;
    BatteryReader& battery_reader_;
    lv_obj_t* clock_label_;
    // Battery percentage and the Wi-Fi icon share this one label (rather
    // than being separately-positioned objects) so LVGL's own text
    // layout keeps their spacing uniform - the same "%s  %d%%" two-space
    // convention RefreshStatusLabel already used internally.
    lv_obj_t* battery_label_;
    // Tracked so a ClockTickEvent-driven refresh (see clock_subscription_
    // below) can still render the Wi-Fi icon correctly between
    // WifiConnectivityChangedEvent transitions.
    bool wifi_connected_;
    // Battery refreshes on Clock's existing tick rather than a second
    // timer - a 1Hz read is more than enough for a percentage display,
    // and reusing the tick already flowing through here matches
    // CLAUDE.md's "use shared scheduling mechanisms where practical" for
    // background work.
    EventBus::ScopedSubscription clock_subscription_;
    // The Wi-Fi icon refreshes on its own event rather than this same
    // tick - connectivity is a discrete, rare transition, not a
    // slowly-drifting number, so tying it to a 1Hz poll would mean
    // near-constant no-op re-renders. See NetworkStatusMonitor.
    EventBus::ScopedSubscription wifi_subscription_;
};

}  // namespace homedeck
