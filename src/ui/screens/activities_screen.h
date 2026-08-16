#pragma once

#include "core/event_bus.h"
#include "core/harmony_connection.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
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
// and clears it once HarmonyCurrentActivityChangedEvent actually
// confirms the switch. A confirming event only fires on an actual
// activity-ID change, so a command that fails to reach the hub (no
// change to detect) would otherwise leave this message stuck forever -
// this screen also clears it on any HarmonyConnectionStateChangedEvent
// away from kConnected, since HarmonyConnection's own connection loop
// treats a failed send the same as a dropped connection and reconnects,
// making a state change a reliable enough signal that the pending tap is
// no longer trustworthy.
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
    // class's own header comment above.
    std::string starting_activity_id_;

    EventBus::ScopedSubscription config_sub_;
    EventBus::ScopedSubscription activity_sub_;
    EventBus::ScopedSubscription state_sub_;
};

}  // namespace homedeck
