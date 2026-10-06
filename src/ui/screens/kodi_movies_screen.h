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

// The Kodi module's Movies library-browse screen (docs/roadmap.md's M4b
// "Media browsing" item, video-library slice) - reached from
// NowPlayingScreen's "Movies" button. One screen, two internal view
// states (movie list, then a selected movie's Play/Resume choice), not
// two Navigation routes - same no-back-stack reasoning as DevicesScreen
// (see its own header comment).
//
// Requests a fresh list on construction and on every transition into
// KodiConnectionState::kConnected (covers both the first connect and any
// later reconnect while this screen exists) via
// KodiClient::RequestMovies(); KodiMoviesFetchedEvent rebuilds the list.
// Each movie is a plain "Title (Year)" label; artwork is out of scope
// (see ADR-0030).
class KodiMoviesScreen {
public:
    KodiMoviesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                      KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~KodiMoviesScreen();

    KodiMoviesScreen(const KodiMoviesScreen&) = delete;
    KodiMoviesScreen& operator=(const KodiMoviesScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();  // hint_label_ vs the rest, per KodiClient::Snapshot()
    void RebuildMovieList(const std::vector<KodiMovie>& movies, bool truncated);
    // Looks the id up in movies_ (the last fetched list) - a stale tap
    // (movies_ changed underneath) is a defensive no-op, not observed.
    void ShowMovieDetail(long long movieid);
    void ShowMovieList();

    static void OnPlayClicked(lv_event_t* e);
    static void OnResumeClicked(lv_event_t* e);
    static void OnBackButtonClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;        // shown instead of everything below when not connected
    lv_obj_t* list_container_;    // == ScreenChrome's content_container - holds movie_list_
    lv_obj_t* detail_container_;  // back button + title + Play/Resume, sibling of list_container_
    lv_obj_t* movie_title_label_;
    lv_obj_t* resume_button_;  // hidden unless the selected movie has a resume point

    // Declared after root_ so it is destroyed before it is: see
    // VirtualList's own lifetime note.
    std::unique_ptr<VirtualList> movie_list_;

    std::vector<KodiMovie> movies_;  // last fetched list - the rows' source and ShowMovieDetail()'s lookup
    long long selected_movie_id_ = -1;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription movies_sub_;
};

}  // namespace homedeck
