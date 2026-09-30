#include "ui/screens/kodi_movies_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

KodiMoviesScreen::KodiMoviesScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                    KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Movies", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    list_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Sibling of list_container_ inside container, not nested inside it -
    // same shape as DevicesScreen's detail_container_ (see its own
    // comment): the two views replace each other via hidden flags rather
    // than one containing the other.
    detail_container_ = lv_obj_create(container);
    lv_obj_remove_style_all(detail_container_);
    lv_obj_set_size(detail_container_, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(detail_container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(detail_container_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(detail_container_, 12, 0);
    lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* back_button = CreateNavChromeButton(detail_container_, LV_SYMBOL_LEFT " Movies");
    lv_obj_add_event_cb(back_button, OnBackButtonClicked, LV_EVENT_CLICKED, this);

    movie_title_label_ = lv_label_create(detail_container_);
    lv_obj_set_width(movie_title_label_, LV_PCT(100));
    lv_label_set_long_mode(movie_title_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(movie_title_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_margin_top(movie_title_label_, 16, 0);

    lv_obj_t* play_button = CreateRemoteButton(detail_container_, "Play");
    lv_obj_add_event_cb(play_button, OnPlayClicked, LV_EVENT_CLICKED, this);

    resume_button_ = CreateRemoteButton(detail_container_, "Resume");
    lv_obj_add_event_cb(resume_button_, OnResumeClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(resume_button_, LV_OBJ_FLAG_HIDDEN);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                kodi_client_.RequestMovies();
            }
        });
    movies_sub_ = event_bus.SubscribeUi<KodiMoviesFetchedEvent>(
        [this](const KodiMoviesFetchedEvent& event) { RebuildMovieList(event.movies); });

    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        kodi_client_.RequestMovies();
    }

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
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        // detail_container_'s own visibility is deliberately untouched -
        // a brief reconnect blip while a movie's Play/Resume choice is
        // showing must not snap the user back to the list.
        if (lv_obj_has_flag(detail_container_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KodiMoviesScreen::RebuildMovieList(const std::vector<KodiMovie>& movies) {
    movies_ = movies;
    lv_obj_clean(list_container_);
    movie_button_ids_.clear();

    if (movies.empty()) {
        lv_obj_t* empty_label = lv_label_create(list_container_);
        lv_label_set_text(empty_label, "No movies in the library.");
        return;
    }

    for (const KodiMovie& movie : movies) {
        std::string label = movie.title;
        if (movie.year > 0) {
            label += " (" + std::to_string(movie.year) + ")";
        }
        lv_obj_t* button = CreateRemoteButton(list_container_, label);
        lv_obj_add_event_cb(button, OnMovieButtonClicked, LV_EVENT_CLICKED, this);
        movie_button_ids_[button] = movie.movieid;
    }
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
        lv_obj_clear_flag(resume_button_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(resume_button_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
}

void KodiMoviesScreen::ShowMovieList() {
    lv_obj_add_flag(detail_container_, LV_OBJ_FLAG_HIDDEN);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_clear_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

void KodiMoviesScreen::OnMovieButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiMoviesScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->movie_button_ids_.find(button);
    if (it == self->movie_button_ids_.end()) {
        return;
    }
    self->ShowMovieDetail(it->second);
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
