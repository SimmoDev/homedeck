#include "ui/screens/activities_screen.h"

#include "ui/home_affordance.h"
#include "ui/remote_button.h"
#include "ui/theme.h"

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
    lv_obj_set_style_text_font(root_, kBodyFont, 0);

    lv_obj_t* container = lv_obj_create(root_);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, StatusBar::kHeight + 16);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 8, 0);
    lv_obj_set_style_pad_all(container, 16, 0);

    lv_obj_t* title = lv_label_create(container);
    lv_label_set_text(title, "Activities");

    status_label_ = lv_label_create(container);
    lv_label_set_text(status_label_, "");  // LVGL defaults a new label's text to "Text" otherwise.

    hint_label_ = lv_label_create(container);
    lv_label_set_text(hint_label_, "Not connected to a Harmony Hub yet.");
    lv_label_set_long_mode(hint_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_label_, LV_PCT(90));
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);

    list_container_ = lv_obj_create(container);
    lv_obj_remove_style_all(list_container_);
    lv_obj_set_size(list_container_, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_container_, LV_FLEX_FLOW_COLUMN);
    // Deliberate spacing between genuinely large buttons, not a tight list -
    // see each button's own pad_ver comment below for why "large" is the
    // deliberate target here, not a compact default. root_ (the screen
    // itself) is left scrollable, same as DashboardGrid's own "content
    // can exceed the visible screen" handling, so a longer activity list
    // still works.
    lv_obj_set_style_pad_row(list_container_, 12, 0);

    lv_obj_t* home_button = CreateHomeAffordance(root_, navigation);

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
        [this](const HarmonyCurrentActivityChangedEvent&) { RestyleButtons(); });

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
    if (current_id == starting_activity_id_) {
        starting_activity_id_.clear();
    }
    lv_label_set_text(status_label_, "");

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

    self->harmony_connection_.StartActivity(activity_id);
    self->starting_activity_id_ = activity_id;

    lv_obj_t* label = lv_obj_get_child(button, 0);
    lv_label_set_text_fmt(self->status_label_, "Starting %s...", lv_label_get_text(label));

    self->RestyleButtons();
}

void ActivitiesScreen::OnDevicesButtonClicked(lv_event_t* e) {
    auto* self = static_cast<ActivitiesScreen*>(lv_event_get_user_data(e));
    self->navigation_.GoTo("harmony-devices");
}

}  // namespace homedeck
