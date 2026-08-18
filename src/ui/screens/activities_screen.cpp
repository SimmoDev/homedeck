#include "ui/screens/activities_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/harmony_screen_chrome.h"

namespace homedeck {

namespace {

// Plain accent color, not a considered design choice - same "placeholder
// styling, considered theme styling is M7 scope" reasoning
// WifiSetupScreen's error_label_ already documents. Green, not LVGL's
// default button blue
// (LV_PALETTE_BLUE) - that would be visually indistinguishable from
// every other, non-current button's own default theme color.
constexpr lv_palette_t kCurrentActivityPalette = LV_PALETTE_GREEN;

}  // namespace

ActivitiesScreen::ActivitiesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                    HarmonyConnection& harmony_connection, Navigation& navigation)
    : harmony_connection_(harmony_connection),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    HarmonyScreenChrome chrome = CreateHarmonyScreenChrome(root_, "Activities", navigation);
    hint_label_ = chrome.hint_label;
    list_container_ = chrome.list_container;
    lv_obj_t* home_button = chrome.home_button;

    // Between the title and hint_label - see CreateHarmonyScreenChrome()'s
    // own comment on why a caller with content to insert there repositions
    // it after the fact rather than the chrome function taking an
    // insertion hook.
    status_label_ = lv_label_create(chrome.container);
    lv_label_set_text(status_label_, "");  // LVGL defaults a new label's text to "Text" otherwise.
    lv_obj_move_to_index(status_label_, 1);

    // A secondary, lighter-weight affordance than the home button/activity
    // buttons - Devices is the advanced/raw-command surface (see
    // DevicesScreen's own header comment), not the primary
    // remote-replacement interaction this screen already is.
    lv_obj_t* devices_button = lv_button_create(root_);
    // Below StatusBar::kHeight, not just root_'s bare top-right corner -
    // that would sit inside the status bar's own chrome.
    lv_obj_align(devices_button, LV_ALIGN_TOP_RIGHT, -16, StatusBar::kHeight + 16);
    // Same reason as CreateHomeAffordance's own FLOATING flag - without
    // it, this button scrolls away with list_container_'s content and
    // drags with root_'s overscroll bounce instead of staying put as
    // fixed chrome.
    lv_obj_add_flag(devices_button, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(devices_button, OnDevicesButtonClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* devices_label = lv_label_create(devices_button);
    lv_label_set_text(devices_label, "Devices");

    config_sub_ = event_bus.SubscribeUi<HarmonyConfigUpdatedEvent>(
        [this](const HarmonyConfigUpdatedEvent&) { Rebuild(); });
    activity_sub_ = event_bus.SubscribeUi<HarmonyCurrentActivityChangedEvent>(
        [this](const HarmonyCurrentActivityChangedEvent&) {
            // The current activity changed - whatever it changed to,
            // any "Starting <name>..." wait this screen was showing is
            // over: either it's the one that was requested, or something
            // else (a second Harmony client) changed it instead - either
            // way, there's nothing left to keep waiting for.
            starting_activity_id_.clear();
            command_failed_ = false;
            RestyleButtons();
        });
    // A pending "Starting <name>..." tap only clears via an activity-ID
    // change (above) - a send that never reached the hub produces no
    // change to detect, so this class's own header comment on why a
    // connection-state change is treated as an equally valid correction
    // signal. Once reconnected, also clears a stale command_failed_
    // message left over from dropped_sub_ below - it's served its
    // purpose once the connection is healthy again, and nothing else
    // would otherwise supersede it until the user taps something new.
    state_sub_ = event_bus.SubscribeUi<HarmonyConnectionStateChangedEvent>(
        [this](const HarmonyConnectionStateChangedEvent& event) {
            if (event.state != HarmonyConnectionState::kConnected && !starting_activity_id_.empty()) {
                starting_activity_id_.clear();
                RestyleButtons();
            } else if (event.state == HarmonyConnectionState::kConnected && command_failed_) {
                command_failed_ = false;
                RestyleButtons();
            }
        });
    // A queued tap that ages out (HarmonyConnection::max_pending_command_age_)
    // before a connection ever comes back to send it over produces
    // neither of the two events above - see this class's own header
    // comment. Reports it explicitly rather than leaving "Starting
    // <name>..." to clear on its own with no indication the tap failed.
    dropped_sub_ = event_bus.SubscribeUi<HarmonyCommandDroppedEvent>([this](const HarmonyCommandDroppedEvent&) {
        if (starting_activity_id_.empty()) return;  // nothing pending right now - not this screen's own drop
        auto it = activity_buttons_.find(starting_activity_id_);
        const char* label = it != activity_buttons_.end() ? lv_label_get_text(lv_obj_get_child(it->second, 0)) : "activity";
        lv_label_set_text_fmt(status_label_, "Couldn't start %s - hub unreachable", label);
        starting_activity_id_.clear();
        command_failed_ = true;
    });

    Rebuild();

    // status_bar_ is constructed before any of this screen's own content
    // (member-initializer order), so list_container_'s scrolled content
    // would otherwise paint over it, and home_button/devices_button would
    // paint under whatever's added after them - see StatusBar::Root()'s
    // own comment. FLOATING (above) only excludes these three from
    // scrolling, not from paint order, which is separate and still
    // follows child-insertion order.
    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
    lv_obj_move_foreground(devices_button);
}

ActivitiesScreen::~ActivitiesScreen() { lv_obj_del(root_); }

void ActivitiesScreen::Rebuild() {
    HarmonyConnectionSnapshot snapshot = harmony_connection_.Snapshot();

    lv_obj_clean(list_container_);
    activity_buttons_.clear();
    button_activity_ids_.clear();

    if (!snapshot.has_config) {
        lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);

    for (const HarmonyActivity& activity : snapshot.activities) {
        lv_obj_t* button = CreateRemoteButton(list_container_, activity.label);
        lv_obj_add_event_cb(button, OnActivityButtonClicked, LV_EVENT_CLICKED, this);

        activity_buttons_[activity.id] = button;
        button_activity_ids_[button] = activity.id;
    }

    RestyleButtons();
}

void ActivitiesScreen::RestyleButtons() {
    std::string current_id = harmony_connection_.Snapshot().current_activity_id;
    // Only blank while nothing is pending and no failure message is
    // showing - called right after OnActivityButtonClicked() sets
    // starting_activity_id_ to show "Starting <name>...", and blanking
    // unconditionally here would erase that in the same synchronous call
    // before it's ever rendered. command_failed_ (dropped_sub_'s own
    // message) is cleared separately, once something actually supersedes
    // it - see its own comment.
    if (starting_activity_id_.empty() && !command_failed_) {
        lv_label_set_text(status_label_, "");
    }

    for (const auto& [id, button] : activity_buttons_) {
        if (id == current_id) {
            lv_obj_set_style_bg_color(button, lv_palette_main(kCurrentActivityPalette), 0);
        } else {
            // Clears this button's own locally-set bg color (if any),
            // falling back to the button theme's default again - not a
            // hardcoded "un-highlight" color, which would need to track
            // whatever the theme's own default happens to be. Plain `0`
            // selector, not LV_PART_MAIN | LV_STATE_DEFAULT - same
            // convention every other lv_obj_set_style_* call in this
            // codebase already uses (both are 0; combining them trips
            // this compiler's enum-mixing warning for no benefit).
            lv_obj_remove_local_style_prop(button, LV_STYLE_BG_COLOR, 0);
        }
    }
}

void ActivitiesScreen::OnActivityButtonClicked(lv_event_t* e) {
    auto* self = static_cast<ActivitiesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->button_activity_ids_.find(button);
    if (it == self->button_activity_ids_.end()) {
        return;
    }
    const std::string& activity_id = it->second;
    if (activity_id == self->harmony_connection_.Snapshot().current_activity_id) {
        // Already the running activity - nothing to start, and showing
        // "Starting <name>..." here would never clear: a same-activity
        // StartActivity() send produces no activity-ID change, so
        // FetchCurrentActivity() never publishes
        // HarmonyCurrentActivityChangedEvent to clear it (that event
        // only fires on an actual ID change - see this screen's own
        // subscription above).
        return;
    }

    self->harmony_connection_.StartActivity(activity_id);
    self->starting_activity_id_ = activity_id;
    self->command_failed_ = false;

    lv_obj_t* label = lv_obj_get_child(button, 0);
    lv_label_set_text_fmt(self->status_label_, "Starting %s...", lv_label_get_text(label));

    self->RestyleButtons();
}

void ActivitiesScreen::OnDevicesButtonClicked(lv_event_t* e) {
    auto* self = static_cast<ActivitiesScreen*>(lv_event_get_user_data(e));
    self->navigation_.GoTo("harmony-devices");
}

}  // namespace homedeck
