#include "ui/screens/devices_screen.h"

#include "ui/home_affordance.h"
#include "ui/remote_button.h"
#include "ui/text_format.h"
#include "ui/theme.h"

namespace homedeck {

namespace {

// Every command grid button is this size, including RenderDPad()'s
// spacers - LV_SIZE_CONTENT (each button auto-fitting its own label)
// looked fine when every button happened to need the same number of
// text lines, but a device with mixed one-line/two-line labels in the
// same row (e.g. "Volume Up" vs "Volume Down") made otherwise-identical
// buttons different heights. 110 = kBodyFont's 27px line height * 2
// (room for a two-line label) + the 28px top/bottom padding
// CreateRemoteButton already applies * 2.
constexpr int32_t kGridButtonWidth = LV_PCT(31);
constexpr int32_t kGridButtonHeight = 110;
constexpr int32_t kGridGap = 12;

// A plain command-name lookup, not a per-device-model special case -
// every name below is common across several of the reference hub's 8
// devices, not specific to one. Deliberately not exhaustive: some
// commands have no icon that uniquely identifies them without also
// being misread as a different command. PowerOff/PowerOn/PowerToggle
// are the clearest case - LVGL has exactly one power icon, and showing
// it on three buttons that do three different things would be worse
// than the text it'd replace. FastForward/Rewind are the other way
// round - no seek icon exists at all, and reusing PREV/NEXT (SkipBackward/
// SkipForward's own icons below) would make both pairs ambiguous on any
// device that has both groups. PrevChannel got the same treatment for
// the inverse reason: it read as a same-family action to ChannelDown's
// own "-" icon (jump back to the last-watched channel, not step down
// one), which text alone didn't make clear.
const char* IconForCommandName(const std::string& name) {
    if (name == "VolumeUp" || name == "ChannelUp") return LV_SYMBOL_PLUS;
    if (name == "VolumeDown" || name == "ChannelDown") return LV_SYMBOL_MINUS;
    if (name == "PrevChannel") return LV_SYMBOL_LOOP;
    if (name == "Mute") return LV_SYMBOL_MUTE;
    if (name == "Play") return LV_SYMBOL_PLAY;
    if (name == "Pause") return LV_SYMBOL_PAUSE;
    if (name == "Stop") return LV_SYMBOL_STOP;
    if (name == "Eject") return LV_SYMBOL_EJECT;
    if (name == "SkipBackward") return LV_SYMBOL_PREV;
    if (name == "SkipForward") return LV_SYMBOL_NEXT;
    if (name == "Home") return LV_SYMBOL_HOME;
    return nullptr;
}

lv_obj_t* CreateRowWrapGrid(lv_obj_t* parent) {
    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, kGridGap, 0);
    lv_obj_set_style_pad_column(grid, kGridGap, 0);
    return grid;
}

lv_obj_t* CreateDPadRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kGridGap, 0);
    return row;
}

// A structurally-empty D-pad corner (top-left, top-right, bottom-left,
// bottom-right) - unconditional, unlike AddDPadCell()'s own placeholder
// for a direction/Select command this specific device happens not to
// have.
void AddDPadSpacer(lv_obj_t* row) {
    lv_obj_t* spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, kGridButtonWidth, kGridButtonHeight);
}

const HarmonyCommand* FindCommand(const std::vector<HarmonyCommand>& commands, const char* name) {
    for (const HarmonyCommand& command : commands) {
        if (command.name == name) return &command;
    }
    return nullptr;
}

}  // namespace

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

    status_label_ = lv_label_create(detail_container_);
    lv_label_set_text(status_label_, "");  // LVGL defaults a new label's text to "Text" otherwise.

    commands_container_ = lv_obj_create(detail_container_);
    lv_obj_remove_style_all(commands_container_);
    lv_obj_set_size(commands_container_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(commands_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(commands_container_, 12, 0);

    lv_obj_t* home_button = CreateHomeAffordance(root_, navigation);

    config_sub_ = event_bus.SubscribeUi<HarmonyConfigUpdatedEvent>(
        [this](const HarmonyConfigUpdatedEvent&) { RebuildDeviceList(); });
    // A command send has no result of its own to check (Press/Hold/
    // ReleaseDeviceCommand() are all void) - this is the only signal
    // available that one just failed, so it's shown regardless of
    // whether kError was actually caused by a command send or something
    // else (e.g. a liveness probe) - both mean the same thing to a user
    // looking at this screen: the connection can't currently be trusted.
    state_sub_ = event_bus.SubscribeUi<HarmonyConnectionStateChangedEvent>([this](const HarmonyConnectionStateChangedEvent& event) {
        if (event.state == HarmonyConnectionState::kError) {
            lv_label_set_text(status_label_, "Connection lost - retrying...");
        } else if (event.state == HarmonyConnectionState::kConnected) {
            lv_label_set_text(status_label_, "");
        }
    });

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
    // The buttons long_press_active_buttons_ refers to are about to be
    // deleted (lv_obj_clean() above) - stale pointers otherwise.
    long_press_active_buttons_.clear();

    for (const HarmonyControlGroup& group : device->control_groups) {
        // A group with no commands is kept in the parsed data (see
        // ParseControlGroups()'s own comment on why), but a heading with
        // nothing under it is pure clutter here - a separate, deliberate
        // rendering-layer choice, not an oversight.
        if (group.commands.empty()) {
            continue;
        }
        RenderControlGroup(group);
    }

    lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
}

void DevicesScreen::RenderControlGroup(const HarmonyControlGroup& group) {
    lv_obj_t* heading = lv_label_create(commands_container_);
    lv_label_set_text(heading, SplitCamelCase(group.name).c_str());
    lv_obj_set_style_text_color(heading, lv_palette_main(LV_PALETTE_GREY), 0);

    if (group.name == "NumericBasic") {
        RenderNumericKeypad(commands_container_, group.commands);
    } else if (group.name == "NavigationBasic") {
        RenderDPad(commands_container_, group.commands);
    } else {
        RenderGenericGrid(commands_container_, group.commands);
    }
}

void DevicesScreen::RenderGenericGrid(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands) {
    // command labels are short and predictable (unlike device/activity
    // labels), so one per row wastes width and, on a device with many
    // commands, turns into a long scroll for no reason. 3 per row is a
    // plain fixed rule (not tailored per group/control type beyond the
    // Numeric/Navigation groups broken out above - see roadmap.md's own
    // reasoning for not going further than that), and still leaves each
    // button comfortably large - the same "large, easy to press"
    // requirement CreateRemoteButton's default shape exists for.
    lv_obj_t* grid = CreateRowWrapGrid(parent);

    for (const HarmonyCommand& command : commands) {
        const char* icon = IconForCommandName(command.name);
        std::string label = icon != nullptr ? std::string(icon) : SplitCamelCase(command.label);
        lv_obj_t* button = CreateRemoteButton(grid, label, kGridButtonWidth, kGridButtonHeight);
        WireCommandButton(button, command.action);
    }
}

void DevicesScreen::RenderNumericKeypad(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands) {
    // Fixed keypad position order, not raw hub order (the hub returns
    // Number0 first, then 1-9) - a numeric keypad reads
    // 1-2-3/4-5-6/7-8-9/Clear-0-Dot.
    static constexpr const char* kKeypadOrder[] = {
        "Number1", "Number2", "Number3", "Number4", "Number5", "Number6",
        "Number7", "Number8", "Number9", "Clear",    "Number0", "Dot",
    };

    lv_obj_t* grid = CreateRowWrapGrid(parent);
    std::unordered_set<std::string> placed;

    for (const char* wanted_name : kKeypadOrder) {
        const HarmonyCommand* command = FindCommand(commands, wanted_name);
        // Not every device has all twelve keys (one device only has
        // Number1-3) - the row-wrap grid just re-flows around a missing
        // one (a keypad without a Dot key has a shorter last row, not
        // an empty cell), unlike RenderDPad()'s cross, where an empty
        // cell would break the shape.
        if (command == nullptr) {
            continue;
        }
        lv_obj_t* button = CreateRemoteButton(grid, command->label, kGridButtonWidth, kGridButtonHeight);
        WireCommandButton(button, command->action);
        placed.insert(wanted_name);
    }

    RenderUnmatchedCommands(parent, commands, placed);
}

void DevicesScreen::RenderDPad(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands) {
    lv_obj_t* rows = lv_obj_create(parent);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rows, kGridGap, 0);

    lv_obj_t* top_row = CreateDPadRow(rows);
    AddDPadSpacer(top_row);
    AddDPadCell(top_row, FindCommand(commands, "DirectionUp"), LV_SYMBOL_UP);
    AddDPadSpacer(top_row);

    lv_obj_t* middle_row = CreateDPadRow(rows);
    AddDPadCell(middle_row, FindCommand(commands, "DirectionLeft"), LV_SYMBOL_LEFT);
    AddDPadCell(middle_row, FindCommand(commands, "Select"), LV_SYMBOL_OK);
    AddDPadCell(middle_row, FindCommand(commands, "DirectionRight"), LV_SYMBOL_RIGHT);

    lv_obj_t* bottom_row = CreateDPadRow(rows);
    AddDPadSpacer(bottom_row);
    AddDPadCell(bottom_row, FindCommand(commands, "DirectionDown"), LV_SYMBOL_DOWN);
    AddDPadSpacer(bottom_row);

    RenderUnmatchedCommands(parent, commands, {"DirectionUp", "DirectionDown", "DirectionLeft", "DirectionRight", "Select"});
}

void DevicesScreen::AddDPadCell(lv_obj_t* row, const HarmonyCommand* command, const char* icon) {
    if (command == nullptr) {
        AddDPadSpacer(row);
        return;
    }
    lv_obj_t* button = CreateRemoteButton(row, icon, kGridButtonWidth, kGridButtonHeight);
    WireCommandButton(button, command->action);
}

void DevicesScreen::WireCommandButton(lv_obj_t* button, const std::string& action) {
    lv_obj_add_event_cb(button, OnCommandButtonLongPressed, LV_EVENT_LONG_PRESSED, this);
    lv_obj_add_event_cb(button, OnCommandButtonLongPressRepeat, LV_EVENT_LONG_PRESSED_REPEAT, this);
    lv_obj_add_event_cb(button, OnCommandButtonReleased, LV_EVENT_RELEASED, this);
    command_button_actions_[button] = action;
}

void DevicesScreen::RenderUnmatchedCommands(lv_obj_t* parent, const std::vector<HarmonyCommand>& commands,
                                             const std::unordered_set<std::string>& matched_names) {
    std::vector<HarmonyCommand> unmatched;
    for (const HarmonyCommand& command : commands) {
        if (matched_names.find(command.name) == matched_names.end()) {
            unmatched.push_back(command);
        }
    }
    if (!unmatched.empty()) {
        RenderGenericGrid(parent, unmatched);
    }
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

void DevicesScreen::OnCommandButtonLongPressed(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }
    self->harmony_connection_.PressDeviceCommand(it->second);
    self->long_press_active_buttons_.insert(button);
}

void DevicesScreen::OnCommandButtonLongPressRepeat(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }
    self->harmony_connection_.HoldDeviceCommand(it->second);
}

void DevicesScreen::OnCommandButtonReleased(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }

    if (self->long_press_active_buttons_.erase(button) > 0) {
        // Was already pressed (and possibly held) - always send the
        // matching release, regardless of what the touch did afterward.
        self->harmony_connection_.ReleaseDeviceCommand(it->second);
        return;
    }

    // Never long-pressed - same check LVGL's own CLICKED implementation
    // makes, done here directly instead of relying on a separate CLICKED
    // handler (see this method's own declaration comment for why).
    if (lv_indev_get_scroll_obj(lv_indev_active()) != nullptr) {
        return;  // this was a drag/scroll that happened to start on the button, not a tap
    }
    self->harmony_connection_.PressDeviceCommand(it->second);
    self->harmony_connection_.ReleaseDeviceCommand(it->second);
}

void DevicesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    self->ShowDeviceList();
}

}  // namespace homedeck
