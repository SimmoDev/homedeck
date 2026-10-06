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
    lv_obj_set_hidden(back_button_, true);  // hidden at the source-list level

    heading_label_ = CreateChromeHeadingLabel(content_);

    list_ = std::make_unique<VirtualList>(content_, "Nothing here.");

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
        lv_obj_set_hidden(hint_label_, true);
        lv_obj_set_hidden(content_, false);
        list_->Refresh();
    } else {
        lv_obj_set_hidden(content_, true);
        lv_obj_set_hidden(hint_label_, false);
        // path_stack_ deliberately untouched - a brief reconnect blip
        // while browsing must not snap the user back to the source
        // list, same reasoning as the fixed-depth screens.
    }
}

void KodiFilesScreen::RequestCurrentLevel() {
    requested_path_ = path_stack_.empty() ? "" : path_stack_.back().first;
    lv_label_set_text(heading_label_, path_stack_.empty() ? "" : path_stack_.back().second.c_str());
    list_->Clear();  // cleared until the fresh KodiFilesFetchedEvent arrives
    if (path_stack_.empty()) {
        lv_obj_set_hidden(back_button_, true);
        kodi_client_.RequestFileSources();
    } else {
        lv_obj_set_hidden(back_button_, false);
        kodi_client_.RequestDirectory(requested_path_);
    }
}

void KodiFilesScreen::RebuildList(const std::vector<KodiFileItem>& items) {
    items_ = items;
    list_->SetItems(
        items_.size(),
        [this](size_t row) {
            const KodiFileItem& item = items_[row];
            return std::string(item.is_folder ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE) + " " + item.label;
        },
        [this](size_t row) { Enter(items_[row]); });
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

void KodiFilesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiFilesScreen*>(lv_event_get_user_data(e));
    self->GoBack();
}

}  // namespace homedeck
