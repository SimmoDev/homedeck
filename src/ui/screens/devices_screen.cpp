#include "ui/screens/devices_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"
#include "ui/text_format.h"

namespace homedeck {

namespace {

// Command grid buttons are 3 per row - see RenderGenericGrid()'s own
// comment. Height comes from remote_button.h's shared kRemoteButtonHeight
// (RenderDPad()'s spacers match it too, so an empty D-pad corner is the
// same size as a real button).
constexpr int32_t kGridButtonWidth = LV_PCT(31);
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
    lv_obj_set_size(spacer, kGridButtonWidth, kRemoteButtonHeight);
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
    ScreenChrome chrome = CreateScreenChrome(root_, "Devices", "Not connected to a Harmony Hub yet.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    list_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // status_label_ sits at the top of the screen (right after the title)
    // rather than inside detail_container_ - visible in both the device
    // list and a device's command view, mirroring ActivitiesScreen's own
    // standing offline indicator (see state_sub_ below).
    status_label_ = CreateChromeStatusLabel(container);

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

    // Same nav-chrome button as the home affordance and ActivitiesScreen's
    // own "Devices" button (CreateNavChromeButton, remote_button.h) -
    // kMinNavTouchTarget-sized with a centred label; this is the only way
    // back from a device's command view short of Home.
    lv_obj_t* back_button = CreateNavChromeButton(detail_container_, LV_SYMBOL_LEFT " Devices");
    lv_obj_add_event_cb(back_button, OnBackButtonClicked, LV_EVENT_CLICKED, this);

    device_title_label_ = lv_label_create(detail_container_);
    // Device labels come straight from the hub's config, not curated
    // strings - a user can name a device anything in the MyHarmony app.
    // detail_container_ is 90%-wide and clips children by default (no
    // LV_OBJ_FLAG_OVERFLOW_VISIBLE set anywhere here), so without an
    // explicit width+wrap a long name would clip mid-word - same pattern
    // screen_chrome.cpp's hint_label already uses.
    lv_obj_set_width(device_title_label_, LV_PCT(90));
    lv_label_set_long_mode(device_title_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(device_title_label_, LV_TEXT_ALIGN_CENTER, 0);

    commands_container_ = lv_obj_create(detail_container_);
    lv_obj_remove_style_all(commands_container_);
    lv_obj_set_size(commands_container_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(commands_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(commands_container_, 12, 0);

    config_sub_ = event_bus.SubscribeUi<HarmonyConfigUpdatedEvent>(
        [this](const HarmonyConfigUpdatedEvent&) { RebuildDeviceList(); });
    // A command send has no result of its own to check (Press/Hold/
    // ReleaseDeviceCommand() are all void), and a silent disconnect/
    // reconnect cycle often never reaches kError at all - HarmonyConnection's
    // own connection loop moves straight from kConnected to kConnecting
    // on a dropped transport, reconnecting before kError is ever set. A
    // kError-only check would miss that common case entirely, so this is
    // a standing indicator for every non-kConnected state instead,
    // matching ActivitiesScreen's own offline indicator (see its
    // RestyleButtons() comment).
    state_sub_ = event_bus.SubscribeUi<HarmonyConnectionStateChangedEvent>([this](const HarmonyConnectionStateChangedEvent& event) {
        lv_label_set_text(status_label_, event.state == HarmonyConnectionState::kConnected
                                              ? ""
                                              : "Offline - reconnecting...");
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

DevicesScreen::~DevicesScreen() {
    // Explicitly unsubscribed first, ahead of member destruction order
    // (config_sub_/state_sub_ are declared last in the header, so
    // they'd otherwise auto-destruct *after* this body runs) - same
    // reasoning as ActivitiesScreen::~ActivitiesScreen(): each
    // callback reads `this`'s own members, so an event dispatched
    // while this destructor is still tearing down root_ must not be
    // able to run against a partially-destroyed screen.
    config_sub_.Reset();
    state_sub_.Reset();
    lv_obj_del(root_);
}

void DevicesScreen::RebuildDeviceList() {
    HarmonyConnectionSnapshot snapshot = harmony_connection_.Snapshot();

    lv_obj_clean(list_container_);
    device_button_ids_.clear();

    if (!snapshot.has_config) {
        lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
        // Also hide detail_container_ (a command view might currently be
        // showing) - otherwise hint_label_ and a now-orphaned screen of
        // command buttons would both be visible at once.
        lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
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
    // The buttons press_tracker_ refers to are about to be deleted
    // (lv_obj_clean() above) - stale pointers otherwise.
    press_tracker_.Reset();

    bool rendered_any_group = false;
    for (const HarmonyControlGroup& group : device->control_groups) {
        // A group with no commands is kept in the parsed data (see
        // ParseControlGroups()'s own comment on why), but a heading with
        // nothing under it is pure clutter here - a separate, deliberate
        // rendering-layer choice, not an oversight.
        if (group.commands.empty()) {
            continue;
        }
        RenderControlGroup(group);
        rendered_any_group = true;
    }

    // A device Harmony knows about but with nothing sendable (every
    // control_groups entry empty) would otherwise leave this view showing
    // just the back button and title with no explanation - every other
    // empty state on these two screens (e.g. hint_label_ for "not
    // connected yet") has one.
    if (!rendered_any_group) {
        lv_obj_t* empty_label = lv_label_create(commands_container_);
        lv_label_set_text(empty_label, "No commands available for this device.");
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
        lv_obj_t* button = CreateRemoteButton(grid, label, kGridButtonWidth);
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
        lv_obj_t* button = CreateRemoteButton(grid, command->label, kGridButtonWidth);
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
    // "OK" as text, not LV_SYMBOL_OK (a checkmark) - Select's own meaning
    // ("confirm/enter") reads more clearly as the word than the icon.
    AddDPadCell(middle_row, FindCommand(commands, "Select"), "OK");
    AddDPadCell(middle_row, FindCommand(commands, "DirectionRight"), LV_SYMBOL_RIGHT);

    lv_obj_t* bottom_row = CreateDPadRow(rows);
    AddDPadSpacer(bottom_row);
    AddDPadCell(bottom_row, FindCommand(commands, "DirectionDown"), LV_SYMBOL_DOWN);
    AddDPadSpacer(bottom_row);

    RenderUnmatchedCommands(parent, commands, {"DirectionUp", "DirectionDown", "DirectionLeft", "DirectionRight", "Select"});
}

void DevicesScreen::AddDPadCell(lv_obj_t* row, const HarmonyCommand* command, const char* label) {
    if (command == nullptr) {
        AddDPadSpacer(row);
        return;
    }
    lv_obj_t* button = CreateRemoteButton(row, label, kGridButtonWidth);
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
    // Only show list_container_ if the hub is still configured -
    // RebuildDeviceList() already hides detail_container_ (and thus this
    // screen's own Back button) the moment the hub becomes unconfigured,
    // so this guard only matters for an already-in-flight tap event
    // racing that transition.
    if (harmony_connection_.Snapshot().has_config) {
        lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    }
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
    // Always kPress - see CommandButtonPressTracker::OnLongPressed()'s own
    // comment.
    self->press_tracker_.OnLongPressed(button);
    self->harmony_connection_.PressDeviceCommand(it->second);
}

void DevicesScreen::OnCommandButtonLongPressRepeat(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }
    self->press_tracker_.OnLongPressRepeat(button);
    self->harmony_connection_.HoldDeviceCommand(it->second);
}

void DevicesScreen::OnCommandButtonReleased(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->command_button_actions_.find(button);
    if (it == self->command_button_actions_.end()) {
        return;
    }

    // Same check LVGL's own CLICKED implementation makes, done here
    // directly instead of relying on a separate CLICKED handler (see this
    // method's own declaration comment for why) - press_tracker_ decides
    // what that means (see its own header comment) since it also needs to
    // remember whether this button was already mid-long-press.
    bool is_scrolling = lv_indev_get_scroll_obj(lv_indev_active()) != nullptr;
    CommandButtonPressTracker::Action action = self->press_tracker_.OnReleased(button, is_scrolling);
    switch (action) {
        case CommandButtonPressTracker::Action::kNone:
            break;  // a drag/scroll that happened to start on the button, not a tap
        case CommandButtonPressTracker::Action::kRelease:
            self->harmony_connection_.ReleaseDeviceCommand(it->second);
            break;
        case CommandButtonPressTracker::Action::kPressAndRelease:
            self->harmony_connection_.PressDeviceCommand(it->second);
            self->harmony_connection_.ReleaseDeviceCommand(it->second);
            break;
        case CommandButtonPressTracker::Action::kPress:
        case CommandButtonPressTracker::Action::kHold:
            break;  // OnReleased() never returns these
    }
}

void DevicesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<DevicesScreen*>(lv_event_get_user_data(e));
    self->ShowDeviceList();
}

}  // namespace homedeck
