#include "ui/screens/kodi_music_screen.h"

#include "ui/kodi_display.h"
#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

namespace {

lv_obj_t* CreateDetailContainer(lv_obj_t* parent) {
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 12, 0);
    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    return container;
}

lv_obj_t* CreateHeadingLabel(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_top(label, 16, 0);
    return label;
}

lv_obj_t* CreateListSubcontainer(lv_obj_t* parent) {
    lv_obj_t* list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 12, 0);
    return list;
}

}  // namespace

KodiMusicScreen::KodiMusicScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Music", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    artists_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Each level below is a sibling of artists_container_ inside
    // container, not nested inside it - same shape as DevicesScreen's
    // detail_container_ (see its own comment): views replace each other
    // via hidden flags rather than one containing the other.
    albums_container_ = CreateDetailContainer(container);
    lv_obj_t* albums_back = CreateNavChromeButton(albums_container_, LV_SYMBOL_LEFT " Artists");
    lv_obj_add_event_cb(albums_back, OnAlbumsBackClicked, LV_EVENT_CLICKED, this);
    albums_title_label_ = CreateHeadingLabel(albums_container_);
    albums_list_ = CreateListSubcontainer(albums_container_);

    songs_container_ = CreateDetailContainer(container);
    lv_obj_t* songs_back = CreateNavChromeButton(songs_container_, LV_SYMBOL_LEFT " Albums");
    lv_obj_add_event_cb(songs_back, OnSongsBackClicked, LV_EVENT_CLICKED, this);
    songs_title_label_ = CreateHeadingLabel(songs_container_);
    songs_list_ = CreateListSubcontainer(songs_container_);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                kodi_client_.RequestArtists();
            }
        });
    artists_sub_ = event_bus.SubscribeUi<KodiArtistsFetchedEvent>(
        [this](const KodiArtistsFetchedEvent& event) { RebuildArtistList(event.artists); });
    // Filtered by the currently-open artist/album - a reply for an artist
    // the user has since backed out of (a slow query racing a fast back
    // tap) must not repopulate a list that's no longer showing.
    albums_sub_ = event_bus.SubscribeUi<KodiAlbumsFetchedEvent>([this](const KodiAlbumsFetchedEvent& event) {
        if (event.artistid == selected_artistid_) {
            RebuildAlbumList(event.albums);
        }
    });
    songs_sub_ = event_bus.SubscribeUi<KodiSongsFetchedEvent>(
        [this](const KodiSongsFetchedEvent& event) { RebuildSongList(event.songs); });

    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        kodi_client_.RequestArtists();
    }

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
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        // Every deeper level's own visibility is deliberately untouched -
        // a brief reconnect blip while browsing albums/songs must not
        // snap the user back to the top level.
        const bool nothing_deeper_showing = lv_obj_has_flag(albums_container_, LV_OBJ_FLAG_HIDDEN) &&
                                            lv_obj_has_flag(songs_container_, LV_OBJ_FLAG_HIDDEN);
        if (nothing_deeper_showing) {
            lv_obj_clear_flag(artists_container_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(artists_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(albums_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(songs_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KodiMusicScreen::RebuildArtistList(const std::vector<KodiArtist>& artists) {
    artists_ = artists;
    lv_obj_clean(artists_container_);
    artist_button_ids_.clear();

    if (artists.empty()) {
        lv_obj_t* empty_label = lv_label_create(artists_container_);
        lv_label_set_text(empty_label, "No artists in the library.");
        return;
    }

    for (const KodiArtist& artist : artists) {
        lv_obj_t* button = CreateRemoteButton(artists_container_, artist.name);
        lv_obj_add_event_cb(button, OnArtistButtonClicked, LV_EVENT_CLICKED, this);
        artist_button_ids_[button] = artist.artistid;
    }
}

void KodiMusicScreen::RebuildAlbumList(const std::vector<KodiAlbum>& albums) {
    lv_obj_clean(albums_list_);
    album_button_ids_.clear();

    if (albums.empty()) {
        lv_obj_t* empty_label = lv_label_create(albums_list_);
        lv_label_set_text(empty_label, "No albums found.");
        return;
    }

    for (const KodiAlbum& album : albums) {
        std::string label = album.title;
        if (album.year > 0) {
            label += " (" + std::to_string(album.year) + ")";
        }
        lv_obj_t* button = CreateRemoteButton(albums_list_, label);
        lv_obj_add_event_cb(button, OnAlbumButtonClicked, LV_EVENT_CLICKED, this);
        album_button_ids_[button] = album.albumid;
    }
}

void KodiMusicScreen::RebuildSongList(const std::vector<KodiSong>& songs) {
    lv_obj_clean(songs_list_);
    song_button_ids_.clear();

    if (songs.empty()) {
        lv_obj_t* empty_label = lv_label_create(songs_list_);
        lv_label_set_text(empty_label, "No songs found.");
        return;
    }

    for (const KodiSong& song : songs) {
        std::string label = std::to_string(song.track) + ". " + song.title + " (" +
                            FormatKodiClock(static_cast<long long>(song.duration_seconds) * 1000) + ")";
        lv_obj_t* button = CreateRemoteButton(songs_list_, label);
        lv_obj_add_event_cb(button, OnSongButtonClicked, LV_EVENT_CLICKED, this);
        song_button_ids_[button] = song.songid;
    }
}

void KodiMusicScreen::ShowArtistList() {
    lv_obj_add_flag(albums_container_, LV_OBJ_FLAG_HIDDEN);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_clear_flag(artists_container_, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_clean(albums_list_);
    album_button_ids_.clear();

    lv_obj_add_flag(artists_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(albums_container_, LV_OBJ_FLAG_HIDDEN);
    kodi_client_.RequestAlbums(artistid);
}

void KodiMusicScreen::ShowSongList(long long albumid) {
    lv_obj_clean(songs_list_);
    song_button_ids_.clear();
    // The album's own title isn't known here (only its id - AlbumButton
    // lookup would need the last-fetched albums_ list, which this screen
    // doesn't keep beyond building the list); the artist name alone is
    // still a useful heading, and the song list itself makes the album
    // clear from its track listing.
    lv_label_set_text(songs_title_label_, selected_artist_name_.c_str());

    lv_obj_add_flag(albums_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(songs_container_, LV_OBJ_FLAG_HIDDEN);
    kodi_client_.RequestSongs(albumid);
}

void KodiMusicScreen::OnArtistButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->artist_button_ids_.find(button);
    if (it == self->artist_button_ids_.end()) {
        return;
    }
    self->ShowAlbumList(it->second);
}

void KodiMusicScreen::OnAlbumButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->album_button_ids_.find(button);
    if (it == self->album_button_ids_.end()) {
        return;
    }
    self->ShowSongList(it->second);
}

void KodiMusicScreen::OnSongButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->song_button_ids_.find(button);
    if (it == self->song_button_ids_.end()) {
        return;
    }
    // No Play/Resume choice - AudioLibrary.GetSongs has no resume
    // property (see this class's own header comment), so a tap plays
    // directly.
    self->kodi_client_.OpenLibraryItem("songid", it->second, /*resume=*/false);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiMusicScreen::OnAlbumsBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    self->ShowArtistList();
}

void KodiMusicScreen::OnSongsBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMusicScreen*>(lv_event_get_user_data(e));
    lv_obj_add_flag(self->songs_container_, LV_OBJ_FLAG_HIDDEN);
    if (self->kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_clear_flag(self->albums_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace homedeck
