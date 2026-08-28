#include "ui/screens/kodi_remote_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

#include <cstdint>

namespace homedeck {

namespace {

constexpr int32_t kCellWidth = LV_PCT(30);
constexpr int32_t kSecondaryWidth = LV_PCT(31);

lv_obj_t* CreateRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    return row;
}

// An empty cell the same footprint as a button, so the D-pad's corners
// stay open without collapsing the cross - matches DevicesScreen's own
// AddDPadSpacer.
void AddSpacer(lv_obj_t* row) {
    lv_obj_t* spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, kCellWidth, kRemoteButtonHeight);
}

}  // namespace

KodiRemoteScreen::KodiRemoteScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Kodi Remote", "Kodi not connected.", navigation);
    hint_label_ = chrome.hint_label;
    content_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    auto add_button = [this](lv_obj_t* row, const char* label, KodiInput input, int32_t width) {
        lv_obj_t* button = CreateRemoteButton(row, label, width);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(input)));
        lv_obj_add_event_cb(button, OnInputClicked, LV_EVENT_CLICKED, this);
    };

    lv_obj_t* top_row = CreateRow(content_);
    AddSpacer(top_row);
    add_button(top_row, LV_SYMBOL_UP, KodiInput::kUp, kCellWidth);
    AddSpacer(top_row);

    lv_obj_t* middle_row = CreateRow(content_);
    add_button(middle_row, LV_SYMBOL_LEFT, KodiInput::kLeft, kCellWidth);
    add_button(middle_row, "OK", KodiInput::kSelect, kCellWidth);
    add_button(middle_row, LV_SYMBOL_RIGHT, KodiInput::kRight, kCellWidth);

    lv_obj_t* bottom_row = CreateRow(content_);
    AddSpacer(bottom_row);
    add_button(bottom_row, LV_SYMBOL_DOWN, KodiInput::kDown, kCellWidth);
    AddSpacer(bottom_row);

    lv_obj_t* secondary = lv_obj_create(content_);
    lv_obj_remove_style_all(secondary);
    lv_obj_set_size(secondary, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(secondary, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(secondary, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(secondary, 12, 0);
    lv_obj_set_style_pad_column(secondary, 12, 0);
    add_button(secondary, LV_SYMBOL_LEFT " Back", KodiInput::kBack, kSecondaryWidth);
    add_button(secondary, LV_SYMBOL_HOME, KodiInput::kHome, kSecondaryWidth);
    add_button(secondary, "Info", KodiInput::kInfo, kSecondaryWidth);
    add_button(secondary, "OSD", KodiInput::kShowOsd, kSecondaryWidth);
    add_button(secondary, "Menu", KodiInput::kContextMenu, kSecondaryWidth);

    Refresh();
    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent&) { Refresh(); });

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiRemoteScreen::~KodiRemoteScreen() {
    state_sub_.Reset();
    lv_obj_del(root_);
}

void KodiRemoteScreen::Refresh() {
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KodiRemoteScreen::OnInputClicked(lv_event_t* e) {
    auto* self = static_cast<KodiRemoteScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto input = static_cast<KodiInput>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    self->kodi_client_.SendInput(input);
}

}  // namespace homedeck
