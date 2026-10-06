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

// The Kodi module's Music library-browse screen (docs/roadmap.md's M4b
// "Media browsing" item, music slice) - reached from NowPlayingScreen's
// "Music" button. Three internal view states (artists -> albums ->
// songs), not three Navigation routes - same no-back-stack reasoning as
// DevicesScreen/KodiMoviesScreen/KodiTvShowsScreen. Unlike the video
// browse screens there is no fourth Play/Resume detail level:
// AudioLibrary.GetSongs has no "resume" property at all, so a song plays
// directly on tap via
// OpenLibraryItem("songid", id, /*resume=*/false).
//
// An album's songs aren't known until that album is chosen, so
// KodiClient::RequestAlbums()/RequestSongs() fire when that level is
// entered, not up front - unlike RequestArtists() itself, which fires on
// construction and on every reconnect (same shape as
// KodiMoviesScreen::RequestMovies()).
class KodiMusicScreen {
public:
    KodiMusicScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                     KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~KodiMusicScreen();

    KodiMusicScreen(const KodiMusicScreen&) = delete;
    KodiMusicScreen& operator=(const KodiMusicScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();  // hint_label_ vs whichever level was showing, per KodiClient::Snapshot()
    void RebuildArtistList(const std::vector<KodiArtist>& artists, bool truncated);
    void RebuildAlbumList(const std::vector<KodiAlbum>& albums, bool truncated);
    void RebuildSongList(const std::vector<KodiSong>& songs, bool truncated);

    void ShowArtistList();
    // Requests fresh albums and switches to albums_container_ - a stale
    // tap (artists_ changed underneath) is a defensive no-op, not
    // observed.
    void ShowAlbumList(long long artistid);
    // Requests fresh songs for the already-open artist and switches to
    // songs_container_.
    void ShowSongList(long long albumid);

    static void OnAlbumsBackClicked(lv_event_t* e);
    static void OnSongsBackClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;
    lv_obj_t* artists_container_;  // == ScreenChrome's content_container - holds artists_list_

    lv_obj_t* albums_container_;  // back button + artist name + albums_list_, sibling of artists_container_
    lv_obj_t* albums_title_label_;

    lv_obj_t* songs_container_;  // back button + "<artist> - <album>" heading + songs_list_
    lv_obj_t* songs_title_label_;

    // Declared after root_ so they are destroyed before it is: see
    // VirtualList's own lifetime note.
    std::unique_ptr<VirtualList> artists_list_;
    std::unique_ptr<VirtualList> albums_list_;
    std::unique_ptr<VirtualList> songs_list_;

    // Last fetched - what each list's rows are read from and what a tap
    // resolves its row index against.
    std::vector<KodiArtist> artists_;
    std::vector<KodiAlbum> albums_;
    std::vector<KodiSong> songs_;

    long long selected_artistid_ = -1;
    std::string selected_artist_name_;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription artists_sub_;
    EventBus::ScopedSubscription albums_sub_;
    EventBus::ScopedSubscription songs_sub_;
};

}  // namespace homedeck
