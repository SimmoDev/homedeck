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
// HarmonyControlGroup's own comment. Sending a command is a press+release
// pair per tap; sustained press-and-hold repeat isn't built - see
// HarmonyConnection's own header comment.
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
    void AddDPadCell(lv_obj_t* row, const HarmonyCommand* command, const char* icon);
    // Commands from a NumericBasic/NavigationBasic group that don't
    // match any of that layout's known positions - not observed on any
    // of the reference hub's 8 devices, but rendering them via the
    // generic grid rather than silently dropping them costs little.
    void RenderUnmatchedCommands(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands,
                                  const std::unordered_set<std::string>& matched_names);

    static void OnDeviceButtonClicked(lv_event_t* e);
    static void OnCommandButtonClicked(lv_event_t* e);
    static void OnBackButtonClicked(lv_event_t* e);

    HarmonyConnection& harmony_connection_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;         // shown instead of the list when has_config is false
    lv_obj_t* list_container_;     // scrollable flex-column of device buttons
    lv_obj_t* detail_container_;   // back button + device title + commands_container_
    lv_obj_t* device_title_label_;
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

    EventBus::ScopedSubscription config_sub_;
};

}  // namespace homedeck
