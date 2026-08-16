#include "ui/screens/devices_screen.h"

#include "ui/home_affordance.h"
#include "ui/remote_button.h"
#include "ui/theme.h"

namespace homedeck {

DevicesScreen::DevicesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                              HarmonyConnection& harmony_connection, Navigation& navigation)
    : harmony_connection_(harmony_connection),
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
    lv_label_set_text(title, "Devices");

    hint_label_ = lv_label_create(container);
    lv_label_set_text(hint_label_, "Not connected to a Harmony Hub yet.");
    lv_label_set_long_mode(hint_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_label_, LV_PCT(90));
    lv_obj_set_style_text_align(hint_label_, LV_TEXT_ALIGN_CENTER, 0);

    list_container_ = lv_obj_create(container);
    lv_obj_remove_style_all(list_container_);
    lv_obj_set_size(list_container_, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_container_, 12, 0);

    detail_container_ = lv_obj_create(container);
    lv_obj_remove_style_all(detail_container_);
    lv_obj_set_size(detail_container_, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(detail_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(detail_container_, 8, 0);
    // Hidden until ShowDeviceDetail() - RebuildDeviceList() never touches
    // this flag itself, only ShowDeviceDetail()/ShowDeviceList() do (see
    // their own comments).
    lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* back_button = lv_button_create(detail_container_);
    lv_obj_add_event_cb(back_button, OnBackButtonClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Devices");

    device_title_label_ = lv_label_create(detail_container_);

    commands_container_ = lv_obj_create(detail_container_);
    lv_obj_remove_style_all(commands_container_);
    lv_obj_set_size(commands_container_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(commands_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(commands_container_, 12, 0);

    lv_obj_t* home_button = CreateHomeAffordance(root_, navigation);

    config_sub_ = event_bus.SubscribeUi<HarmonyConfigUpdatedEvent>(
        [this](const HarmonyConfigUpdatedEvent&) { RebuildDeviceList(); });

    RebuildDeviceList();

    // status_bar_ is constructed before any of this screen's own content
    // (member-initializer order), so list_container_/commands_container_'s
    // scrolled content would otherwise paint over it - see
    // StatusBar::Root()'s own comment. FLOATING (see CreateHomeAffordance's
    // own comment) only excludes the bar/home_button from scrolling, not
    // from paint order, which is separate and still follows
    // child-insertion order.
    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

DevicesScreen::~DevicesScreen() { lv_obj_del(root_); }

void DevicesScreen::RebuildDeviceList() {
    HarmonyConnectionSnapshot snapshot = harmony_connection_.Snapshot();

    lv_obj_clean(list_container_);
    device_button_ids_.clear();

    if (!snapshot.has_config) {
        lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    // detail_container_'s own visibility is deliberately untouched here -
    // see this function's own header comment.
    if (lv_obj_has_flag(detail_container_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    }

    for (const HarmonyDevice& device : snapshot.devices) {
        lv_obj_t* button = CreateRemoteButton(list_container_, device.label);
        lv_obj_add_event_cb(button, OnDeviceButtonClicked, LV_EVENT_CLICKED, this);
        device_button_ids_[button] = device.id;
    }
}

void DevicesScreen::ShowDeviceDetail(const std::string& device_id) {
    HarmonyConnectionSnapshot snapshot = harmony_connection_.Snapshot();
    const HarmonyDevice* device = nullptr;
    for (const HarmonyDevice& candidate : snapshot.devices) {
        if (candidate.id == device_id) {
            device = &candidate;
            break;
        }
    }
    if (device == nullptr) {
        return;  // stale tap (device list changed underneath) - defensive, not an observed case
    }

    lv_label_set_text(device_title_label_, device->label.c_str());

    lv_obj_clean(commands_container_);
    command_button_actions_.clear();

    for (const HarmonyControlGroup& group : device->control_groups) {
        // A group with no commands is kept in the parsed data (see
        // ParseControlGroups()'s own comment on why), but a heading with
        // nothing under it is pure clutter here - a separate, deliberate
        // rendering-layer choice, not an oversight.
        if (group.commands.empty()) {
            continue;
        }
        lv_obj_t* heading = lv_label_create(commands_container_);
        lv_label_set_text(heading, group.name.c_str());
        lv_obj_set_style_text_color(heading, lv_palette_main(LV_PALETTE_GREY), 0);

        // A row-wrap grid, not commands_container_'s own single column -
        // command labels are short and predictable (unlike device/activity
        // labels), so one per row wastes width and, on a device with many
        // commands, turns into a long scroll for no reason. 3 per row is a
        // plain fixed rule (not tailored per group/control type - see
        // roadmap.md's own reasoning for avoiding that), and still leaves
        // each button comfortably large - the same "large, easy to press"
        // requirement CreateRemoteButton's default shape exists for.
        lv_obj_t* group_grid = lv_obj_create(commands_container_);
        lv_obj_remove_style_all(group_grid);
        lv_obj_set_size(group_grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(group_grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(group_grid, 12, 0);
        lv_obj_set_style_pad_column(group_grid, 12, 0);

        for (const HarmonyCommand& command : group.commands) {
            // 31%, not a third - room for the two 12px column gaps above
            // between three buttons on the same row without overflowing.
            lv_obj_t* button = CreateRemoteButton(group_grid, command.label, LV_PCT(31));
            lv_obj_add_event_cb(button, OnCommandButtonClicked, LV_EVENT_CLICKED, this);
            command_button_actions_[button] = command.action;
        }
    }

    lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
}

void DevicesScreen::ShowDeviceList() {
    lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
}

void DevicesScreen::OnDeviceButtonClicked(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->device_button_ids_.find(button);
    if (it == self->device_button_ids_.end()) {
        return;
    }
    self->ShowDeviceDetail(it->second);
}

void DevicesScreen::OnCommandButtonClicked(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }
    self->harmony_connection_.SendDeviceCommand(it->second);
}

void DevicesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    self->ShowDeviceList();
}

}  // namespace homedeck
