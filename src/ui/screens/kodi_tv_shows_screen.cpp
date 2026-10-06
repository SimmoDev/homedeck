#include "ui/screens/kodi_tv_shows_screen.h"

#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

namespace {

// CreateChromeDetailContainer()/CreateChromeHeadingLabel()/
// CreateChromeListSubcontainer() (screen_chrome.h) cover the object
// setup this used to duplicate locally - this screen's own pad_row (12)
// is passed through unchanged.

std::string SeasonHeading(const std::string& show_title, int season) {
    return show_title + (season == 0 ? " - Specials" : " - Season " + std::to_string(season));
}

}  // namespace

KodiTvShowsScreen::KodiTvShowsScreen(EventBus& event_bus, BatteryReader& battery_reader,
                                      NetworkStatus& network_status, KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "TV Shows", "Kodi not connected.", navigation);
    lv_obj_t* container = chrome.container;
    hint_label_ = chrome.hint_label;
    shows_container_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    // Each level below is a sibling of shows_container_ inside container,
    // not nested inside it - same shape as DevicesScreen's
    // detail_container_ (see its own comment): views replace each other
    // via hidden flags rather than one containing the other.
    seasons_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* seasons_back = CreateNavChromeButton(seasons_container_, LV_SYMBOL_LEFT " TV Shows");
    lv_obj_add_event_cb(seasons_back, OnSeasonsBackClicked, LV_EVENT_CLICKED, this);
    seasons_title_label_ = CreateChromeHeadingLabel(seasons_container_);
    seasons_list_ = CreateChromeListSubcontainer(seasons_container_, 12);

    episodes_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* episodes_back = CreateNavChromeButton(episodes_container_, LV_SYMBOL_LEFT " Seasons");
    lv_obj_add_event_cb(episodes_back, OnEpisodesBackClicked, LV_EVENT_CLICKED, this);
    episodes_title_label_ = CreateChromeHeadingLabel(episodes_container_);
    episodes_list_ = CreateChromeListSubcontainer(episodes_container_, 12);

    episode_detail_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* episode_detail_back = CreateNavChromeButton(episode_detail_container_, LV_SYMBOL_LEFT " Episodes");
    lv_obj_add_event_cb(episode_detail_back, OnEpisodeDetailBackClicked, LV_EVENT_CLICKED, this);
    episode_title_label_ = CreateChromeHeadingLabel(episode_detail_container_);

    lv_obj_t* play_button = CreateRemoteButton(episode_detail_container_, "Play");
    lv_obj_add_event_cb(play_button, OnPlayClicked, LV_EVENT_CLICKED, this);

    resume_button_ = CreateRemoteButton(episode_detail_container_, "Resume");
    lv_obj_add_event_cb(resume_button_, OnResumeClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(resume_button_, true);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                kodi_client_.RequestTvShows();
            }
        });
    shows_sub_ = event_bus.SubscribeUi<KodiTvShowsFetchedEvent>(
        [this](const KodiTvShowsFetchedEvent& event) { RebuildShowList(event.shows); });
    // Filtered by the currently-open show/season - a reply for a show the
    // user has since backed out of (a slow query racing a fast back tap)
    // must not repopulate a list that's no longer showing.
    seasons_sub_ = event_bus.SubscribeUi<KodiSeasonsFetchedEvent>([this](const KodiSeasonsFetchedEvent& event) {
        if (event.tvshowid == selected_tvshowid_) {
            RebuildSeasonList(event.seasons);
        }
    });
    episodes_sub_ = event_bus.SubscribeUi<KodiEpisodesFetchedEvent>([this](const KodiEpisodesFetchedEvent& event) {
        if (event.tvshowid == selected_tvshowid_ && event.season == selected_season_) {
            RebuildEpisodeList(event.episodes);
        }
    });

    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        kodi_client_.RequestTvShows();
    }

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

KodiTvShowsScreen::~KodiTvShowsScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as DevicesScreen's destructor).
    state_sub_.Reset();
    shows_sub_.Reset();
    seasons_sub_.Reset();
    episodes_sub_.Reset();
    lv_obj_del(root_);
}

void KodiTvShowsScreen::Refresh() {
    const bool connected = kodi_client_.Snapshot().state == KodiConnectionState::kConnected;
    if (connected) {
        lv_obj_set_hidden(hint_label_, true);
        // Every deeper level's own visibility is deliberately untouched -
        // a brief reconnect blip while browsing seasons/episodes must not
        // snap the user back to the top level.
        const bool nothing_deeper_showing = lv_obj_is_hidden(seasons_container_) &&
                                            lv_obj_is_hidden(episodes_container_) &&
                                            lv_obj_is_hidden(episode_detail_container_);
        if (nothing_deeper_showing) {
            lv_obj_set_hidden(shows_container_, false);
        }
    } else {
        lv_obj_set_hidden(shows_container_, true);
        lv_obj_set_hidden(seasons_container_, true);
        lv_obj_set_hidden(episodes_container_, true);
        lv_obj_set_hidden(episode_detail_container_, true);
        lv_obj_set_hidden(hint_label_, false);
    }
}

void KodiTvShowsScreen::RebuildShowList(const std::vector<KodiTvShow>& shows) {
    shows_ = shows;
    lv_obj_clean(shows_container_);
    show_button_ids_.clear();

    if (shows.empty()) {
        lv_obj_t* empty_label = lv_label_create(shows_container_);
        lv_label_set_text(empty_label, "No TV shows in the library.");
        return;
    }

    for (const KodiTvShow& show : shows) {
        std::string label = show.title;
        if (show.year > 0) {
            label += " (" + std::to_string(show.year) + ")";
        }
        lv_obj_t* button = CreateRemoteButton(shows_container_, label);
        lv_obj_add_event_cb(button, OnShowButtonClicked, LV_EVENT_CLICKED, this);
        show_button_ids_[button] = show.tvshowid;
    }
}

void KodiTvShowsScreen::RebuildSeasonList(const std::vector<KodiSeason>& seasons) {
    lv_obj_clean(seasons_list_);
    season_button_numbers_.clear();

    if (seasons.empty()) {
        lv_obj_t* empty_label = lv_label_create(seasons_list_);
        lv_label_set_text(empty_label, "No seasons found.");
        return;
    }

    for (const KodiSeason& season : seasons) {
        lv_obj_t* button = CreateRemoteButton(seasons_list_, season.label);
        lv_obj_add_event_cb(button, OnSeasonButtonClicked, LV_EVENT_CLICKED, this);
        season_button_numbers_[button] = season.season;
    }
}

void KodiTvShowsScreen::RebuildEpisodeList(const std::vector<KodiEpisode>& episodes) {
    episodes_ = episodes;
    lv_obj_clean(episodes_list_);
    episode_button_ids_.clear();

    if (episodes.empty()) {
        lv_obj_t* empty_label = lv_label_create(episodes_list_);
        lv_label_set_text(empty_label, "No episodes found.");
        return;
    }

    for (const KodiEpisode& episode : episodes) {
        std::string label = std::to_string(episode.episode) + ". " + episode.title;
        lv_obj_t* button = CreateRemoteButton(episodes_list_, label);
        lv_obj_add_event_cb(button, OnEpisodeButtonClicked, LV_EVENT_CLICKED, this);
        episode_button_ids_[button] = episode.episodeid;
    }
}

void KodiTvShowsScreen::ShowShowList() {
    lv_obj_set_hidden(seasons_container_, true);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(shows_container_, false);
    }
}

void KodiTvShowsScreen::ShowSeasonList(long long tvshowid) {
    const KodiTvShow* show = nullptr;
    for (const KodiTvShow& candidate : shows_) {
        if (candidate.tvshowid == tvshowid) {
            show = &candidate;
            break;
        }
    }
    if (show == nullptr) {
        return;
    }

    selected_tvshowid_ = tvshowid;
    selected_show_title_ = show->title;
    lv_label_set_text(seasons_title_label_, show->title.c_str());
    // Cleared until the fresh KodiSeasonsFetchedEvent arrives - showing
    // the previous show's stale season buttons for one frame would be
    // worse than a brief empty list.
    lv_obj_clean(seasons_list_);
    season_button_numbers_.clear();

    lv_obj_set_hidden(shows_container_, true);
    lv_obj_set_hidden(seasons_container_, false);
    kodi_client_.RequestSeasons(tvshowid);
}

void KodiTvShowsScreen::ShowEpisodeList(int season) {
    selected_season_ = season;
    lv_label_set_text(episodes_title_label_, SeasonHeading(selected_show_title_, season).c_str());
    lv_obj_clean(episodes_list_);
    episode_button_ids_.clear();

    lv_obj_set_hidden(seasons_container_, true);
    lv_obj_set_hidden(episodes_container_, false);
    kodi_client_.RequestEpisodes(selected_tvshowid_, season);
}

void KodiTvShowsScreen::ShowEpisodeDetail(long long episodeid) {
    const KodiEpisode* episode = nullptr;
    for (const KodiEpisode& candidate : episodes_) {
        if (candidate.episodeid == episodeid) {
            episode = &candidate;
            break;
        }
    }
    if (episode == nullptr) {
        return;
    }

    selected_episode_id_ = episodeid;
    lv_label_set_text(episode_title_label_, episode->title.c_str());
    if (episode->resume_position_ms > 0) {
        lv_obj_set_hidden(resume_button_, false);
    } else {
        lv_obj_set_hidden(resume_button_, true);
    }

    lv_obj_set_hidden(episodes_container_, true);
    lv_obj_set_hidden(episode_detail_container_, false);
}

void KodiTvShowsScreen::OnShowButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->show_button_ids_.find(button);
    if (it == self->show_button_ids_.end()) {
        return;
    }
    self->ShowSeasonList(it->second);
}

void KodiTvShowsScreen::OnSeasonButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->season_button_numbers_.find(button);
    if (it == self->season_button_numbers_.end()) {
        return;
    }
    self->ShowEpisodeList(it->second);
}

void KodiTvShowsScreen::OnEpisodeButtonClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));

    auto it = self->episode_button_ids_.find(button);
    if (it == self->episode_button_ids_.end()) {
        return;
    }
    self->ShowEpisodeDetail(it->second);
}

void KodiTvShowsScreen::OnPlayClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("episodeid", self->selected_episode_id_, /*resume=*/false);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiTvShowsScreen::OnResumeClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("episodeid", self->selected_episode_id_, /*resume=*/true);
    self->navigation_.GoTo("kodi-now-playing");
}

void KodiTvShowsScreen::OnSeasonsBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    self->ShowShowList();
}

void KodiTvShowsScreen::OnEpisodesBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    lv_obj_set_hidden(self->episodes_container_, true);
    if (self->kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(self->seasons_container_, false);
    }
}

void KodiTvShowsScreen::OnEpisodeDetailBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    lv_obj_set_hidden(self->episode_detail_container_, true);
    if (self->kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(self->episodes_container_, false);
    }
}

}  // namespace homedeck
