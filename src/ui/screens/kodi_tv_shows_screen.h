#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"
#include "ui/virtual_list.h"

#include <memory>
#include <string>
#include <vector>

namespace homedeck {

// The Kodi module's TV Shows library-browse screen (docs/roadmap.md's M4b
// "Media browsing" item, video-library slice) - reached from
// NowPlayingScreen's "TV Shows" button. One screen, four internal view
// states (shows -> seasons -> episodes -> a selected episode's
// Play/Resume choice), not four Navigation routes - same no-back-stack
// reasoning as DevicesScreen/KodiMoviesScreen. Each "back" button goes up
// exactly one level, never straight to the show list or Home.
//
// A show's seasons, and a season's episodes, aren't known until that
// show/season is chosen, so KodiClient::RequestSeasons()/RequestEpisodes()
// fire when that level is entered, not up front - unlike RequestTvShows()
// itself, which fires on construction and on every reconnect (same
// shape as KodiMoviesScreen::RequestMovies()).
class KodiTvShowsScreen {
public:
    KodiTvShowsScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                       KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~KodiTvShowsScreen();

    KodiTvShowsScreen(const KodiTvShowsScreen&) = delete;
    KodiTvShowsScreen& operator=(const KodiTvShowsScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();  // hint_label_ vs whichever level was showing, per KodiClient::Snapshot()
    void RebuildShowList(const std::vector<KodiTvShow>& shows, bool truncated);
    void RebuildSeasonList(const std::vector<KodiSeason>& seasons, bool truncated);
    void RebuildEpisodeList(const std::vector<KodiEpisode>& episodes, bool truncated);

    void ShowShowList();
    // Requests fresh seasons and switches to seasons_container_ - a
    // stale tap (shows_ changed underneath) is a defensive no-op, not
    // observed.
    void ShowSeasonList(long long tvshowid);
    // Requests fresh episodes for the already-open show and switches to
    // episodes_container_.
    void ShowEpisodeList(int season);
    // Looks episodeid up in episodes_ (the last fetched list).
    void ShowEpisodeDetail(long long episodeid);

    static void OnPlayClicked(lv_event_t* e);
    static void OnResumeClicked(lv_event_t* e);
    static void OnSeasonsBackClicked(lv_event_t* e);
    static void OnEpisodesBackClicked(lv_event_t* e);
    static void OnEpisodeDetailBackClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;
    lv_obj_t* shows_container_;  // == ScreenChrome's content_container - holds shows_list_

    lv_obj_t* seasons_container_;  // back button + show title + seasons_list_, sibling of shows_container_
    lv_obj_t* seasons_title_label_;

    lv_obj_t* episodes_container_;  // back button + "<show> - <season>" heading + episodes_list_
    lv_obj_t* episodes_title_label_;

    lv_obj_t* episode_detail_container_;  // back button + episode title + Play/Resume
    lv_obj_t* episode_title_label_;
    lv_obj_t* resume_button_;  // hidden unless the selected episode has a resume point

    // Declared after root_ so they are destroyed before it is: see
    // VirtualList's own lifetime note.
    std::unique_ptr<VirtualList> shows_list_;
    std::unique_ptr<VirtualList> seasons_list_;
    std::unique_ptr<VirtualList> episodes_list_;

    // Last fetched - what each list's rows are read from and what a tap
    // resolves its row index against.
    std::vector<KodiTvShow> shows_;
    std::vector<KodiSeason> seasons_;
    std::vector<KodiEpisode> episodes_;

    long long selected_tvshowid_ = -1;
    std::string selected_show_title_;
    int selected_season_ = 0;
    long long selected_episode_id_ = -1;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription shows_sub_;
    EventBus::ScopedSubscription seasons_sub_;
    EventBus::ScopedSubscription episodes_sub_;
};

}  // namespace homedeck
