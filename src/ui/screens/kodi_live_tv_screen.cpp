#include "ui/screens/kodi_live_tv_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

// CreateChromeDetailContainer()/CreateChromeHeadingLabel()/
// CreateChromeListSubcontainer() (screen_chrome.h) cover the object
// setup this used to duplicate locally.

KodiLiveTvScreen::KodiLiveTvScreen(EventBus& event_bus, BatteryReader& battery_reader,
                                    NetworkStatus& network_status, KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Live TV", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    groups_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Sibling of groups_container_ inside container, not nested inside
    // it - same shape as DevicesScreen's detail_container_ (see its own
    // comment): the two views replace each other via hidden flags
    // rather than one containing the other.
    channels_container_ = CreateChromeDetailContainer(container, 8);

    lv_obj_t* back_button = CreateNavChromeButton(channels_container_, LV_SYMBOL_LEFT " Live TV");
    lv_obj_add_event_cb(back_button, OnBackButtonClicked, LV_EVENT_CLICKED, this);

    channels_title_label_ = CreateChromeHeadingLabel(channels_container_);

    channels_list_ = CreateChromeListSubcontainer(channels_container_, 12);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                kodi_client_.RequestChannelGroups();
            }
        });
    groups_sub_ = event_bus.SubscribeUi<KodiChannelGroupsFetchedEvent>(
        [this](const KodiChannelGroupsFetchedEvent& event) { RebuildGroupList(event.groups); });
    // Filtered by the currently-open group - a reply for a group the
    // user has since backed out of (a slow query racing a fast back
    // tap) must not repopulate a list that's no longer showing.
    channels_sub_ = event_bus.SubscribeUi<KodiChannelsFetchedEvent>([this](const KodiChannelsFetchedEvent& event) {
        if (event.channelgroupid == selected_channelgroupid_) {
            RebuildChannelList(event.channels);
        }
    });

    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        kodi_client_.RequestChannelGroups();
    }

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiLiveTvScreen::~KodiLiveTvScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as DevicesScreen's destructor).
    state_sub_.Reset();
    groups_sub_.Reset();
    channels_sub_.Reset();
    lv_obj_del(root_);
}

void KodiLiveTvScreen::Refresh() {
    const bool connected = kodi_client_.Snapshot().state == KodiConnectionState::kConnected;
    if (connected) {
        lv_obj_set_hidden(hint_label_, true);
        // channels_container_'s own visibility is deliberately untouched
        // - a brief reconnect blip while browsing channels must not snap
        // the user back to the group list.
        if (lv_obj_is_hidden(channels_container_)) {
            lv_obj_set_hidden(groups_container_, false);
        }
    } else {
        lv_obj_set_hidden(groups_container_, true);
        lv_obj_set_hidden(channels_container_, true);
        lv_obj_set_hidden(hint_label_, false);
    }
}

void KodiLiveTvScreen::RebuildGroupList(const std::vector<KodiChannelGroup>& groups) {
    groups_ = groups;
    lv_obj_clean(groups_container_);
    group_button_ids_.clear();

    if (groups.empty()) {
        lv_obj_t* empty_label = lv_label_create(groups_container_);
        lv_label_set_text(empty_label, "No channel groups configured.");
        return;
    }

    for (const KodiChannelGroup& group : groups) {
        lv_obj_t* button = CreateRemoteButton(groups_container_, group.label);
        lv_obj_add_event_cb(button, OnGroupButtonClicked, LV_EVENT_CLICKED, this);
        group_button_ids_[button] = group.channelgroupid;
    }
}

void KodiLiveTvScreen::RebuildChannelList(const std::vector<KodiChannel>& channels) {
    lv_obj_clean(channels_list_);
    channel_button_ids_.clear();

    if (channels.empty()) {
        lv_obj_t* empty_label = lv_label_create(channels_list_);
        lv_label_set_text(empty_label, "No channels in this group.");
        return;
    }

    for (const KodiChannel& channel : channels) {
        lv_obj_t* button = CreateRemoteButton(channels_list_, channel.label);
        lv_obj_add_event_cb(button, OnChannelButtonClicked, LV_EVENT_CLICKED, this);
        channel_button_ids_[button] = channel.channelid;
    }
}

void KodiLiveTvScreen::ShowGroupList() {
    lv_obj_set_hidden(channels_container_, true);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(groups_container_, false);
    }
}

void KodiLiveTvScreen::ShowChannelList(long long channelgroupid) {
    const KodiChannelGroup* group = nullptr;
    for (const KodiChannelGroup& candidate : groups_) {
        if (candidate.channelgroupid == channelgroupid) {
            group = &candidate;
            break;
        }
    }
    if (group == nullptr) {
        return;
    }

    selected_channelgroupid_ = channelgroupid;
    lv_label_set_text(channels_title_label_, group->label.c_str());
    // Cleared until the fresh KodiChannelsFetchedEvent arrives - showing
    // the previous group's stale channel buttons for one frame would be
    // worse than a brief empty list.
    lv_obj_clean(channels_list_);
    channel_button_ids_.clear();

    lv_obj_set_hidden(groups_container_, true);
    lv_obj_set_hidden(channels_container_, false);
    kodi_client_.RequestChannels(channelgroupid);
}

void KodiLiveTvScreen::OnGroupButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiLiveTvScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->group_button_ids_.find(button);
    if (it == self->group_button_ids_.end()) {
        return;
    }
    self->ShowChannelList(it->second);
}

void KodiLiveTvScreen::OnChannelButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiLiveTvScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->channel_button_ids_.find(button);
    if (it == self->channel_button_ids_.end()) {
        return;
    }
    // No Play/Resume choice - a live broadcast has no resume point (see
    // this class's own header comment).
    self->kodi_client_.OpenLibraryItem("channelid", it->second, /*resume=*/false);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiLiveTvScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiLiveTvScreen*>(lv_event_get_user_data(e));
    self->ShowGroupList();
}

}  // namespace homedeck
