#include "ui/screens/kodi_files_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

KodiFilesScreen::KodiFilesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Files", "Kodi not connected.", navigation);
    hint_label_ = chrome.hint_label;
    content_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Reused across every level (unbounded folder depth, unlike the
    // fixed-depth browse screens - see this class's own header comment)
    // rather than one container per level.
    back_button_ = CreateNavChromeButton(content_, LV_SYMBOL_LEFT " Back");
    lv_obj_add_event_cb(back_button_, OnBackButtonClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(back_button_, LV_OBJ_FLAG_HIDDEN);  // hidden at the source-list level

    heading_label_ = CreateChromeHeadingLabel(content_);

    list_ = CreateChromeListSubcontainer(content_, 12);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                RequestCurrentLevel();
            }
        });
    // Filtered by the currently-requested path - a reply for a level
    // the user has since navigated away from (a slow query racing a
    // fast back/enter tap) must not repopulate a list that's no longer
    // showing, same reasoning as the fixed-depth screens' tvshowid/
    // season checks.
    files_sub_ = event_bus.SubscribeUi<KodiFilesFetchedEvent>([this](const KodiFilesFetchedEvent& event) {
        if (event.path == requested_path_) {
            RebuildList(event.items);
        }
    });

    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        RequestCurrentLevel();
    }

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiFilesScreen::~KodiFilesScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as DevicesScreen's destructor).
    state_sub_.Reset();
    files_sub_.Reset();
    lv_obj_del(root_);
}

void KodiFilesScreen::Refresh() {
    const bool connected = kodi_client_.Snapshot().state == KodiConnectionState::kConnected;
    if (connected) {
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        // path_stack_ deliberately untouched - a brief reconnect blip
        // while browsing must not snap the user back to the source
        // list, same reasoning as the fixed-depth screens.
    }
}

void KodiFilesScreen::RequestCurrentLevel() {
    requested_path_ = path_stack_.empty() ? "" : path_stack_.back().first;
    lv_label_set_text(heading_label_, path_stack_.empty() ? "" : path_stack_.back().second.c_str());
    lv_obj_clean(list_);  // cleared until the fresh KodiFilesFetchedEvent arrives
    item_button_indices_.clear();
    if (path_stack_.empty()) {
        lv_obj_add_flag(back_button_, LV_OBJ_FLAG_HIDDEN);
        kodi_client_.RequestFileSources();
    } else {
        lv_obj_clear_flag(back_button_, LV_OBJ_FLAG_HIDDEN);
        kodi_client_.RequestDirectory(requested_path_);
    }
}

void KodiFilesScreen::RebuildList(const std::vector<KodiFileItem>& items) {
    items_ = items;
    lv_obj_clean(list_);
    item_button_indices_.clear();

    if (items.empty()) {
        lv_obj_t* empty_label = lv_label_create(list_);
        lv_label_set_text(empty_label, "Nothing here.");
        return;
    }

    for (size_t i = 0; i < items.size(); ++i) {
        const KodiFileItem& item = items[i];
        std::string label = std::string(item.is_folder ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE) + " " + item.label;
        lv_obj_t* button = CreateRemoteButton(list_, label);
        lv_obj_add_event_cb(button, OnItemButtonClicked, LV_EVENT_CLICKED, this);
        item_button_indices_[button] = i;
    }
}

void KodiFilesScreen::Enter(const KodiFileItem& item) {
    if (item.is_folder) {
        path_stack_.emplace_back(item.path, item.label);
        RequestCurrentLevel();
    } else {
        kodi_client_.PlayFile(item.path);
        navigation_.GoTo("kodi-now-playing");
    }
}

void KodiFilesScreen::GoBack() {
    if (path_stack_.empty()) {
        return;  // stale tap - the back button is hidden at this level (defensive, not observed)
    }
    path_stack_.pop_back();
    RequestCurrentLevel();
}

void KodiFilesScreen::OnItemButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiFilesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->item_button_indices_.find(button);
    if (it == self->item_button_indices_.end() || it->second >= self->items_.size()) {
        return;
    }
    self->Enter(self->items_[it->second]);
}

void KodiFilesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiFilesScreen*>(lv_event_get_user_data(e));
    self->GoBack();
}

}  // namespace homedeck
