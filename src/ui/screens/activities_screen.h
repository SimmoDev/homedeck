#pragma once

#include "core/event_bus.h"
#include "core/harmony_connection.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/activity_start_tracker.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"

#include <string>
#include <unordered_map>

namespace homedeck {

// The Harmony module's Activities screen (docs/roadmap.md's M3
// "Activities" item) - lists activities, starts one on tap, and shows
// which one is current. Reached by tapping HarmonyWidget
// (src/ui/harmony_widget.h) on the dashboard, not a launcher grid - see
// ADR-0004.
//
// Current-activity freshness after a tap is best-effort, not push-driven
// - see HarmonyConnection's own header comment for why. This screen
// shows an optimistic local "Starting <name>..." status line right away
// and clears it once HarmonyCurrentActivityChangedEvent fires again, for
// any activity - not only the one requested, since a second Harmony
// client (the physical remote, another app) changing the hub's activity
// to something else entirely means the wait is equally over, just not
// with the outcome this screen was expecting. A command that fails to
// reach the hub produces no such event at all, so it would otherwise
// leave this message stuck forever - this screen also clears it on any
// HarmonyConnectionStateChangedEvent away from kConnected, since
// HarmonyConnection's own connection loop treats a failed send the same
// as a dropped connection and reconnects, making a state change a
// reliable enough signal that the pending tap is no longer trustworthy.
//
// A tap made while already disconnected/reconnecting is still queued
// (HarmonyConnection::StartActivity() harmlessly waits for a connection
// to send it over - see that class's own comment), and the state-change
// path above only fires on a *transition*, not on staying disconnected -
// so a queued tap that ages out past max_pending_command_age_ before a
// connection ever comes back produces neither a state change nor an
// activity change to clear "Starting <name>..." with. This screen's own
// dropped_sub_ (HarmonyCommandDroppedEvent) exists specifically for that
// case: an explicit "couldn't start" message, not a silently-cleared one
// that leaves the user with no idea the tap never took effect.
//
// A command that reaches the hub successfully but that the hub then
// silently ignores/rejects (plausible over one-way IR) produces none of
// the above: no state change, no drop event, and no activity-ID change
// either, since the hub's own idea of the current activity never moved.
// starting_timeout_timer_ is the last-resort backstop for exactly this
// case - see its own comment.
//
// Freshness is poll-driven, not push-driven: current_activity_id only
// updates on HarmonyConnection's liveness-probe cycle (30s by default),
// so a legitimately slow-but-working activity start can take nearly
// that long to show up here even on success - see this screen's own
// starting_timeout_timer_ comment for why its own timeout is set well
// above that.
//
// Tapping the already-running activity resends its own start command
// rather than being a no-op - useful to resync AV gear that's drifted
// out of sync with the hub's own idea of what's on, matching the
// official Harmony app/remote's own re-trigger support. See
// OnActivityButtonClicked()'s own comment for the one wrinkle this
// creates (no activity-ID change ever follows a same-activity resend, so
// its confirmation message has no natural point to clear itself).
class ActivitiesScreen {
public:
    ActivitiesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                      HarmonyConnection& harmony_connection, Navigation& navigation);
    // root_ has no owning parent (see ui.md#object-lifecycle) - deleting
    // it recursively deletes status_bar_'s LVGL objects too, since it's
    // root_'s child.
    ~ActivitiesScreen();

    ActivitiesScreen(const ActivitiesScreen&) = delete;
    ActivitiesScreen& operator=(const ActivitiesScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    // Full teardown/rebuild of the activity button list - called at
    // construction and on HarmonyConfigUpdatedEvent (the activity list
    // itself may have changed). HarmonyConnectionStateChangedEvent does
    // *not* trigger this: a momentary disconnect/reconnect keeps showing
    // the last-known activity list, same "keep showing last-known state"
    // policy HarmonyConnectionSnapshot's own comment documents.
    void Rebuild();
    // Recolors the current activity's button and updates/clears the
    // "Starting..." status line - called on HarmonyCurrentActivityChangedEvent
    // and right after a tap, without touching the button list itself.
    void RestyleButtons();
    static void OnActivityButtonClicked(lv_event_t* e);
    static void OnDevicesButtonClicked(lv_event_t* e);
    // Fires starting_timeout_ms after a fresh "Starting <name>..." tap if
    // nothing else has cleared starting_activity_id_ by then - the
    // backstop for a command the hub silently ignores (see this class's
    // own header comment). A single reused one-shot timer, reset on each
    // fresh tap, the same pattern NotificationBanner's dismiss_timer_
    // uses.
    static void OnStartingTimeout(lv_timer_t* timer);

    HarmonyConnection& harmony_connection_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* status_label_;    // "Starting <name>..." or blank
    lv_obj_t* hint_label_;      // shown instead of the list when has_config is false
    lv_obj_t* list_container_;  // scrollable flex-column of activity buttons

    // Both keyed/valued together in Rebuild(), cleared and rebuilt as a
    // pair every time - id -> button for RestyleButtons()'s color pass,
    // button -> id for OnActivityButtonClicked() (LVGL event callbacks
    // only carry one user_data pointer, registered once with `this`, so
    // the clicked lv_obj_t* itself is the other half of the lookup - see
    // lv_event_get_target()'s use in the .cpp).
    std::unordered_map<std::string, lv_obj_t*> activity_buttons_;
    std::unordered_map<lv_obj_t*, std::string> button_activity_ids_;

    // Local optimistic "tapped, not yet confirmed" state - see this
    // class's own header comment above and ActivityStartTracker's own
    // header comment for why this decision logic lives in its own
    // LVGL-free, host-testable class rather than directly in this screen.
    ActivityStartTracker tracker_;
    // Backstop for a command the hub silently ignores - see
    // OnStartingTimeout()'s own comment. Created once, reused for the
    // life of this instance; starts paused.
    lv_timer_t* starting_timeout_timer_;

    EventBus::ScopedSubscription config_sub_;
    EventBus::ScopedSubscription activity_sub_;
    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription dropped_sub_;
};

}  // namespace homedeck
