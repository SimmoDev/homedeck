#include "ui/screens/kodi_music_screen.h"

#include "ui/kodi_display.h"
#include "ui/remote_button.h"
#include "ui/routes.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

KodiMusicScreen::KodiMusicScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      loader_(
          root_, [this] { return kodi_client_.IsConnected(); }, [this] { kodi_client_.RequestArtists(); }) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Music", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    artists_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Each level below is a sibling of artists_container_ inside
    // container, not nested inside it - same shape as DevicesScreen's
    // detail_container_ (see its own comment): views replace each other
    // via hidden flags rather than one containing the other.
    albums_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* albums_back = CreateNavChromeButton(albums_container_, LV_SYMBOL_LEFT " Artists");
    lv_obj_add_event_cb(albums_back, OnAlbumsBackClicked, LV_EVENT_CLICKED, this);
    albums_title_label_ = CreateChromeHeadingLabel(albums_container_);
    albums_list_ = std::make_unique<VirtualList>(albums_container_, "No albums found.");

    songs_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* songs_back = CreateNavChromeButton(songs_container_, LV_SYMBOL_LEFT " Albums");
    lv_obj_add_event_cb(songs_back, OnSongsBackClicked, LV_EVENT_CLICKED, this);
    songs_title_label_ = CreateChromeHeadingLabel(songs_container_);
    songs_list_ = std::make_unique<VirtualList>(songs_container_, "No songs found.");
    artists_list_ = std::make_unique<VirtualList>(artists_container_, "No artists in the library.");

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                loader_.OnConnected();
            }
        });
    artists_sub_ = event_bus.SubscribeUi<KodiArtistsFetchedEvent>(
        [this](const KodiArtistsFetchedEvent& event) { RebuildArtistList(event.artists, event.truncated); });
    // Filtered by the currently-open artist/album - a reply for an artist
    // the user has since backed out of (a slow query racing a fast back
    // tap) must not repopulate a list that's no longer showing.
    albums_sub_ = event_bus.SubscribeUi<KodiAlbumsFetchedEvent>([this](const KodiAlbumsFetchedEvent& event) {
        if (event.artistid == selected_artistid_) {
            RebuildAlbumList(event.albums, event.truncated);
        }
    });
    songs_sub_ = event_bus.SubscribeUi<KodiSongsFetchedEvent>(
        [this](const KodiSongsFetchedEvent& event) { RebuildSongList(event.songs, event.truncated); });

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiMusicScreen::~KodiMusicScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as DevicesScreen's destructor).
    state_sub_.Reset();
    artists_sub_.Reset();
    albums_sub_.Reset();
    songs_sub_.Reset();
    lv_obj_del(root_);
}

void KodiMusicScreen::Refresh() {
    const bool connected = kodi_client_.Snapshot().state == KodiConnectionState::kConnected;
    if (connected) {
        lv_obj_set_hidden(hint_label_, true);
        // A request in flight when the link dropped is lost, so a
        // (re)connect returns to the artist list, which the ScreenLoader
        // reloads.
        lv_obj_set_hidden(albums_container_, true);
        lv_obj_set_hidden(songs_container_, true);
        lv_obj_set_hidden(artists_container_, false);
        artists_list_->Refresh();
    } else {
        lv_obj_set_hidden(artists_container_, true);
        lv_obj_set_hidden(albums_container_, true);
        lv_obj_set_hidden(songs_container_, true);
        lv_obj_set_hidden(hint_label_, false);
    }
}

void KodiMusicScreen::RebuildArtistList(const std::vector<KodiArtist>& artists, bool truncated) {
    artists_ = artists;
    artists_list_->SetItems(
        artists_.size(), [this](size_t row) { return artists_[row].name; },
        [this](size_t row) { ShowAlbumList(artists_[row].artistid); });
    artists_list_->SetTruncated(truncated);
}

void KodiMusicScreen::RebuildAlbumList(const std::vector<KodiAlbum>& albums, bool truncated) {
    albums_ = albums;
    albums_list_->SetItems(
        albums_.size(),
        [this](size_t row) {
            const KodiAlbum& album = albums_[row];
            return album.year > 0 ? album.title + " (" + std::to_string(album.year) + ")" : album.title;
        },
        [this](size_t row) { ShowSongList(albums_[row].albumid); });
    albums_list_->SetTruncated(truncated);
}

void KodiMusicScreen::RebuildSongList(const std::vector<KodiSong>& songs, bool truncated) {
    songs_ = songs;
    // No Play/Resume choice - AudioLibrary.GetSongs has no resume
    // property (see this class's own header comment), so a tap plays
    // directly.
    songs_list_->SetItems(
        songs_.size(),
        [this](size_t row) {
            const KodiSong& song = songs_[row];
            return std::to_string(song.track) + ". " + song.title + " (" +
                   FormatKodiClock(static_cast<long long>(song.duration_seconds) * 1000) + ")";
        },
        [this](size_t row) {
            kodi_client_.OpenLibraryItem("songid", songs_[row].songid, /*resume=*/false);
            navigation_.GoTo(routes::kKodiNowPlaying);
        });
    songs_list_->SetTruncated(truncated);
}

void KodiMusicScreen::ShowArtistList() {
    lv_obj_set_hidden(albums_container_, true);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(artists_container_, false);
        artists_list_->Refresh();
    }
}

void KodiMusicScreen::ShowAlbumList(long long artistid) {
    const KodiArtist* artist = nullptr;
    for (const KodiArtist& candidate : artists_) {
        if (candidate.artistid == artistid) {
            artist = &candidate;
            break;
        }
    }
    if (artist == nullptr) {
        return;
    }

    selected_artistid_ = artistid;
    selected_artist_name_ = artist->name;
    lv_label_set_text(albums_title_label_, artist->name.c_str());
    // Cleared until the fresh KodiAlbumsFetchedEvent arrives - showing
    // the previous artist's stale album buttons for one frame would be
    // worse than a brief empty list.
    albums_list_->Clear();
    albums_.clear();

    lv_obj_set_hidden(artists_container_, true);
    lv_obj_set_hidden(albums_container_, false);
    kodi_client_.RequestAlbums(artistid);
}

void KodiMusicScreen::ShowSongList(long long albumid) {
    songs_list_->Clear();
    songs_.clear();
    // The album's own title isn't known here (only its id - AlbumButton
    // lookup would need the last-fetched albums_ list, which this screen
    // doesn't keep beyond building the list); the artist name alone is
    // still a useful heading, and the song list itself makes the album
    // clear from its track listing.
    lv_label_set_text(songs_title_label_, selected_artist_name_.c_str());

    lv_obj_set_hidden(albums_container_, true);
    lv_obj_set_hidden(songs_container_, false);
    kodi_client_.RequestSongs(albumid);
}

void KodiMusicScreen::OnAlbumsBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    self->ShowArtistList();
}

void KodiMusicScreen::OnSongsBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    lv_obj_set_hidden(self->songs_container_, true);
    if (self->kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(self->albums_container_, false);
        self->albums_list_->Refresh();
    }
}

}  // namespace homedeck
