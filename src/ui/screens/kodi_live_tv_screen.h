#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace homedeck {

// The Kodi module's Live TV browse screen (docs/roadmap.md's M4b
// "Media browsing" item, live-TV slice) - reached from NowPlayingScreen's
// "Live TV" button. Two internal view states (channel groups ->
// channels), not two Navigation routes - same no-back-stack reasoning as
// every other Kodi browse screen. Shallower than every other browse type
// (no third level): there is nothing to scope a channel by beyond its
// group. Scoped to channeltype "tv" - radio, a separate PVR channel
// type Kodi also supports, isn't browsed here.
//
// No Play/Resume choice: a live broadcast has no resume point, so a
// channel plays directly on tap via the existing
// OpenLibraryItem("channelid", id, /*resume=*/false) - the same generic
// id_field mechanism the video screens use for movieid/episodeid, not a
// new command.
class KodiLiveTvScreen {
public:
    KodiLiveTvScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                      KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~KodiLiveTvScreen();

    KodiLiveTvScreen(const KodiLiveTvScreen&) = delete;
    KodiLiveTvScreen& operator=(const KodiLiveTvScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();  // hint_label_ vs whichever level was showing, per KodiClient::Snapshot()
    void RebuildGroupList(const std::vector<KodiChannelGroup>& groups);
    void RebuildChannelList(const std::vector<KodiChannel>& channels);

    void ShowGroupList();
    // Requests fresh channels and switches to channels_container_ - a
    // stale tap (groups_ changed underneath) is a defensive no-op, not
    // observed.
    void ShowChannelList(long long channelgroupid);

    static void OnGroupButtonClicked(lv_event_t* e);
    static void OnChannelButtonClicked(lv_event_t* e);
    static void OnBackButtonClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;
    lv_obj_t* groups_container_;  // == ScreenChrome's content_container - scrollable group buttons

    lv_obj_t* channels_container_;  // back button + group label + channels_list_, sibling of groups_container_
    lv_obj_t* channels_title_label_;
    lv_obj_t* channels_list_;

    std::vector<KodiChannelGroup> groups_;  // last fetched - ShowChannelList()'s lookup source

    // button -> id, same lookup pattern DevicesScreen's device_button_ids_ uses.
    std::unordered_map<lv_obj_t*, long long> group_button_ids_;
    std::unordered_map<lv_obj_t*, long long> channel_button_ids_;

    long long selected_channelgroupid_ = -1;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription groups_sub_;
    EventBus::ScopedSubscription channels_sub_;
};

}  // namespace homedeck
