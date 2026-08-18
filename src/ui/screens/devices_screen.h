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
#include <unordered_set>
#include <vector>

namespace homedeck {

// The Harmony module's Devices screen (docs/roadmap.md's M3 "Devices"
// and "Remote control" items - the same screen covers both, see that
// entry for why: a device's capabilities and its remote-control commands
// are the same data, `controlGroup`/`function`, not two separate things
// to build). The advanced/raw-command surface, one level down from
// ActivitiesScreen (reached via its "Devices" button) - most day-to-day
// use is Activities; this is for when a specific device command is
// actually needed.
//
// One screen, two internal view states (device list, then a selected
// device's commands), not two Navigation routes - this project's
// Navigation has no back-stack (see roadmap.md's M7 Gesture navigation
// item), so a second route would need its own bespoke "back" handling
// for no benefit over a local state toggle.
//
// "Power state" (a device's own Capabilities/powerFeatures fields) isn't
// shown - empty on every device this project has seen against the
// reference hub (IR is one-way, nothing to poll) - see
// HarmonyControlGroup's own comment. A tap sends a press+release pair;
// holding a command button past LVGL's own long-press threshold sends a
// press, then repeated holds for as long as it's held, then a release on
// lift - see OnCommandButtonReleased()'s own comment for the exact
// mechanics (touch-and-drag safety in particular).
class DevicesScreen {
public:
    DevicesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                   HarmonyConnection& harmony_connection, Navigation& navigation);
    // root_ has no owning parent (see ui.md#object-lifecycle) - deleting
    // it recursively deletes status_bar_'s LVGL objects too, since it's
    // root_'s child.
    ~DevicesScreen();

    DevicesScreen(const DevicesScreen&) = delete;
    DevicesScreen& operator=(const DevicesScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    // Full teardown/rebuild of the device list - called at construction
    // and on HarmonyConfigUpdatedEvent. Doesn't touch whichever device's
    // command view might currently be showing (see ShowDeviceDetail()) -
    // a mid-session reconfigure is rare enough that staying put with the
    // already-loaded command list is an acceptable simplification over
    // forcing back to the list.
    void RebuildDeviceList();
    // Switches to the command view for one device, (re)building its
    // command buttons from the current Snapshot() - safe to call again
    // for the same device (e.g. nothing else triggers a rebuild of this
    // view, so there's no live-update need beyond re-deriving it fresh
    // each time it's entered).
    void ShowDeviceDetail(const std::string& device_id);
    void ShowDeviceList();

    // One control group's commands, dispatched by HarmonyControlGroup::name
    // to whichever of the three below matches its own observed shape
    // (see each one's own comment) - a plain row-wrap grid otherwise.
    // Matching by name is a protocol-level vocabulary Harmony's own hub
    // uses consistently across every device that has these groups
    // (confirmed against all 8 devices on the reference hub, not just
    // one), not the kind of per-device-model hardcoding CLAUDE.md warns
    // against.
    void RenderControlGroup(const HarmonyControlGroup& group);
    void RenderGenericGrid(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands);
    // NumericBasic: Number0-Number9 plus optional Clear/Dot, in raw hub
    // order (0 first) - reordered into a keypad's 1-2-3/4-5-6/7-8-9/
    // Clear-0-Dot shape instead. Not every device has all twelve - one
    // device only has Number1-3 - whichever exist just render in this
    // order.
    void RenderNumericKeypad(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands);
    // NavigationBasic: consistently exactly DirectionUp/Down/Left/Right +
    // Select across every device that has it. A cross, not a row-wrap
    // grid - the four corners need to stay empty, which a reflowing grid
    // can't do on its own.
    void RenderDPad(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands);
    // command, if present, or an empty placeholder occupying the same
    // space (so the cross stays a cross even when e.g. Select is
    // missing) - shared by RenderDPad()'s three rows.
    void AddDPadCell(lv_obj_t* row, const HarmonyCommand* command, const char* label);
    // Commands from a NumericBasic/NavigationBasic group that don't
    // match any of that layout's known positions - not observed on any
    // of the reference hub's 8 devices, but rendering them via the
    // generic grid rather than silently dropping them costs little.
    void RenderUnmatchedCommands(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands,
                                  const std::unordered_set<std::string>& matched_names);
    // Registers the long-press/release event trio (see
    // OnCommandButtonReleased()'s own comment) and records `action` in
    // command_button_actions_ - shared by every place a command button
    // gets created (RenderGenericGrid(), RenderNumericKeypad(),
    // AddDPadCell()).
    void WireCommandButton(lv_obj_t* button, const std::string& action);

    static void OnDeviceButtonClicked(lv_event_t* e);
    // LV_EVENT_LONG_PRESSED - fires once, and (per LVGL's own indev.c)
    // only if this press hasn't already turned into a scroll. Sends the
    // command's initial press and marks the button as mid-long-press for
    // OnCommandButtonReleased() below.
    static void OnCommandButtonLongPressed(lv_event_t* e);
    // LV_EVENT_LONG_PRESSED_REPEAT - fires repeatedly (LVGL's own
    // long_press_repeat_time) after LONG_PRESSED, same scroll exclusion.
    // Sends one hold per firing.
    static void OnCommandButtonLongPressRepeat(lv_event_t* e);
    // LV_EVENT_RELEASED, not CLICKED - a drag that starts on a command
    // button and turns into a scroll must send nothing at all (the same
    // safety a plain CLICKED-driven press+release already had), which
    // rules out driving the initial press from LV_EVENT_PRESSED: PRESSED
    // fires immediately on touch-down, before LVGL can know whether the
    // touch will become a scroll, so a scroll starting on a button would
    // send a press with no way to take it back. long_press_active_buttons_
    // (set only from LONG_PRESSED, which - confirmed against LVGL's own
    // indev.c - it never sends once a scroll starts) exists to avoid
    // that: if a button is marked, this is a genuine long press, so a
    // release always gets sent here regardless of what the touch did
    // afterward (dragging while still holding must not leave the device
    // thinking the button is stuck down). If it isn't marked,
    // lv_indev_get_scroll_obj() - the same check LVGL's own CLICKED
    // implementation makes - tells a plain tap (send press+release) apart
    // from a scroll that happened to start on this button (send
    // nothing).
    static void OnCommandButtonReleased(lv_event_t* e);
    static void OnBackButtonClicked(lv_event_t* e);

    HarmonyConnection& harmony_connection_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;         // shown instead of the list when has_config is false
    lv_obj_t* list_container_;     // scrollable flex-column of device buttons
    lv_obj_t* detail_container_;   // back button + device title + status_label_ + commands_container_
    lv_obj_t* device_title_label_;
    // A command send has no synchronous result to wait on (unlike
    // ActivitiesScreen's "Starting <name>..." label, which clears once
    // the activity it's waiting on actually changes) - this only ever
    // shows/clears in response to HarmonyConnectionStateChangedEvent
    // (see state_sub_ below), since that's the only signal a send
    // failure actually produces.
    lv_obj_t* status_label_;
    lv_obj_t* commands_container_;  // group headings + command buttons for the selected device

    // button -> device id, for OnDeviceButtonClicked() (LVGL event
    // callbacks only carry one user_data pointer, registered once with
    // `this`, so the clicked lv_obj_t* itself is the other half of the
    // lookup - see lv_event_get_target()'s use in the .cpp, same pattern
    // ActivitiesScreen already established).
    std::unordered_map<lv_obj_t*, std::string> device_button_ids_;
    // button -> the command's own action string, rebuilt fresh every
    // ShowDeviceDetail() call.
    std::unordered_map<lv_obj_t*, std::string> command_button_actions_;
    // Buttons currently mid-long-press (OnCommandButtonLongPressed() set
    // it, OnCommandButtonReleased() clears it) - see the latter's own
    // comment. Cleared alongside command_button_actions_ on every
    // ShowDeviceDetail() rebuild, not just erased per-button on release,
    // since the buttons themselves get deleted then too.
    std::unordered_set<lv_obj_t*> long_press_active_buttons_;

    EventBus::ScopedSubscription config_sub_;
    EventBus::ScopedSubscription state_sub_;
};

}  // namespace homedeck
