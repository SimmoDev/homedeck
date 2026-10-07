#include "ui/screens/kodi_movies_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

KodiMoviesScreen::KodiMoviesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                    KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      loader_(
          root_, [this] { return kodi_client_.IsConnected(); }, [this] { kodi_client_.RequestMovies(); }) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Movies", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    list_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Sibling of list_container_ inside container, not nested inside it -
    // same shape as DevicesScreen's detail_container_ (see its own
    // comment): the two views replace each other via hidden flags rather
    // than one containing the other.
    movie_list_ = std::make_unique<VirtualList>(list_container_, "No movies in the library.");

    detail_container_ = CreateChromeDetailContainer(container, 12);

    lv_obj_t* back_button = CreateNavChromeButton(detail_container_, LV_SYMBOL_LEFT " Movies");
    lv_obj_add_event_cb(back_button, OnBackButtonClicked, LV_EVENT_CLICKED, this);

    movie_title_label_ = CreateChromeHeadingLabel(detail_container_);

    lv_obj_t* play_button = CreateRemoteButton(detail_container_, "Play");
    lv_obj_add_event_cb(play_button, OnPlayClicked, LV_EVENT_CLICKED, this);

    resume_button_ = CreateRemoteButton(detail_container_, "Resume");
    lv_obj_add_event_cb(resume_button_, OnResumeClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(resume_button_, true);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                loader_.OnConnected();
            }
        });
    movies_sub_ = event_bus.SubscribeUi<KodiMoviesFetchedEvent>(
        [this](const KodiMoviesFetchedEvent& event) { RebuildMovieList(event.movies, event.truncated); });

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiMoviesScreen::~KodiMoviesScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as DevicesScreen's destructor).
    state_sub_.Reset();
    movies_sub_.Reset();
    lv_obj_del(root_);
}

void KodiMoviesScreen::Refresh() {
    const bool connected = kodi_client_.Snapshot().state == KodiConnectionState::kConnected;
    if (connected) {
        lv_obj_set_hidden(hint_label_, true);
        // detail_container_'s own visibility is deliberately untouched -
        // a brief reconnect blip while a movie's Play/Resume choice is
        // showing must not snap the user back to the list.
        if (lv_obj_is_hidden(detail_container_)) {
            lv_obj_set_hidden(list_container_, false);
            movie_list_->Refresh();
        }
    } else {
        lv_obj_set_hidden(list_container_, true);
        lv_obj_set_hidden(detail_container_, true);
        lv_obj_set_hidden(hint_label_, false);
    }
}

void KodiMoviesScreen::RebuildMovieList(const std::vector<KodiMovie>& movies, bool truncated) {
    movies_ = movies;
    movie_list_->SetItems(
        movies_.size(),
        [this](size_t row) {
            const KodiMovie& movie = movies_[row];
            return movie.year > 0 ? movie.title + " (" + std::to_string(movie.year) + ")" : movie.title;
        },
        [this](size_t row) { ShowMovieDetail(movies_[row].movieid); });
    movie_list_->SetTruncated(truncated);
}

void KodiMoviesScreen::ShowMovieDetail(long long movieid) {
    const KodiMovie* movie = nullptr;
    for (const KodiMovie& candidate : movies_) {
        if (candidate.movieid == movieid) {
            movie = &candidate;
            break;
        }
    }
    if (movie == nullptr) {
        return;
    }

    selected_movie_id_ = movieid;
    lv_label_set_text(movie_title_label_, movie->title.c_str());
    if (movie->resume_position_ms > 0) {
        lv_obj_set_hidden(resume_button_, false);
    } else {
        lv_obj_set_hidden(resume_button_, true);
    }

    lv_obj_set_hidden(list_container_, true);
    lv_obj_set_hidden(detail_container_, false);
}

void KodiMoviesScreen::ShowMovieList() {
    lv_obj_set_hidden(detail_container_, true);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(list_container_, false);
        movie_list_->Refresh();
    }
}

void KodiMoviesScreen::OnPlayClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMoviesScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("movieid", self->selected_movie_id_, /*resume=*/false);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiMoviesScreen::OnResumeClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMoviesScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("movieid", self->selected_movie_id_, /*resume=*/true);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiMoviesScreen::OnBackButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMoviesScreen*>(lv_event_get_user_data(e));
    self->ShowMovieList();
}

}  // namespace homedeck
