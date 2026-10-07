#include "ui/screens/kodi_tv_shows_screen.h"

#include "ui/remote_button.h"
#include "ui/routes.h"
#include "ui/screens/screen_chrome.h"

namespace homedeck {

namespace {

std::string SeasonHeading(const std::string& show_title, int season) {
    return show_title + (season == 0 ? " - Specials" : " - Season " + std::to_string(season));
}

}  // namespace

KodiTvShowsScreen::KodiTvShowsScreen(EventBus& event_bus, BatteryReader& battery_reader,
                                      NetworkStatus& network_status, KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      loader_(
          root_, [this] { return kodi_client_.IsConnected(); }, [this] { kodi_client_.RequestTvShows(); }) {
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
    seasons_list_ = std::make_unique<VirtualList>(seasons_container_, "No seasons found.");

    episodes_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* episodes_back = CreateNavChromeButton(episodes_container_, LV_SYMBOL_LEFT " Seasons");
    lv_obj_add_event_cb(episodes_back, OnEpisodesBackClicked, LV_EVENT_CLICKED, this);
    episodes_title_label_ = CreateChromeHeadingLabel(episodes_container_);
    episodes_list_ = std::make_unique<VirtualList>(episodes_container_, "No episodes found.");

    episode_detail_container_ = CreateChromeDetailContainer(container, 12);
    lv_obj_t* episode_detail_back = CreateNavChromeButton(episode_detail_container_, LV_SYMBOL_LEFT " Episodes");
    lv_obj_add_event_cb(episode_detail_back, OnEpisodeDetailBackClicked, LV_EVENT_CLICKED, this);
    episode_title_label_ = CreateChromeHeadingLabel(episode_detail_container_);

    lv_obj_t* play_button = CreateRemoteButton(episode_detail_container_, "Play");
    lv_obj_add_event_cb(play_button, OnPlayClicked, LV_EVENT_CLICKED, this);

    resume_button_ = CreateRemoteButton(episode_detail_container_, "Resume");
    lv_obj_add_event_cb(resume_button_, OnResumeClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_hidden(resume_button_, true);

    shows_list_ = std::make_unique<VirtualList>(shows_container_, "No TV shows in the library.");

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent& event) {
            Refresh();
            if (event.state == KodiConnectionState::kConnected) {
                loader_.OnConnected();
            }
        });
    shows_sub_ = event_bus.SubscribeUi<KodiTvShowsFetchedEvent>(
        [this](const KodiTvShowsFetchedEvent& event) { RebuildShowList(event.shows, event.truncated); });
    // Filtered by the currently-open show/season - a reply for a show the
    // user has since backed out of (a slow query racing a fast back tap)
    // must not repopulate a list that's no longer showing.
    seasons_sub_ = event_bus.SubscribeUi<KodiSeasonsFetchedEvent>([this](const KodiSeasonsFetchedEvent& event) {
        if (event.tvshowid == selected_tvshowid_) {
            RebuildSeasonList(event.seasons, event.truncated);
        }
    });
    episodes_sub_ = event_bus.SubscribeUi<KodiEpisodesFetchedEvent>([this](const KodiEpisodesFetchedEvent& event) {
        if (event.tvshowid == selected_tvshowid_ && event.season == selected_season_) {
            RebuildEpisodeList(event.episodes, event.truncated);
        }
    });

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
        // A request in flight when the link dropped is lost, so a
        // (re)connect returns to the show list, which the ScreenLoader
        // reloads.
        lv_obj_set_hidden(seasons_container_, true);
        lv_obj_set_hidden(episodes_container_, true);
        lv_obj_set_hidden(episode_detail_container_, true);
        lv_obj_set_hidden(shows_container_, false);
        shows_list_->Refresh();
    } else {
        lv_obj_set_hidden(shows_container_, true);
        lv_obj_set_hidden(seasons_container_, true);
        lv_obj_set_hidden(episodes_container_, true);
        lv_obj_set_hidden(episode_detail_container_, true);
        lv_obj_set_hidden(hint_label_, false);
    }
}

void KodiTvShowsScreen::RebuildShowList(const std::vector<KodiTvShow>& shows, bool truncated) {
    loader_.OnLoaded(shows.size(), truncated);
    shows_ = shows;
    shows_list_->SetItems(
        shows_.size(),
        [this](size_t row) {
            const KodiTvShow& show = shows_[row];
            return show.year > 0 ? show.title + " (" + std::to_string(show.year) + ")" : show.title;
        },
        [this](size_t row) { ShowSeasonList(shows_[row].tvshowid); });
    shows_list_->SetTruncated(truncated);
}

void KodiTvShowsScreen::RebuildSeasonList(const std::vector<KodiSeason>& seasons, bool truncated) {
    seasons_ = seasons;
    seasons_list_->SetItems(
        seasons_.size(), [this](size_t row) { return seasons_[row].label; },
        [this](size_t row) { ShowEpisodeList(seasons_[row].season); });
    seasons_list_->SetTruncated(truncated);
}

void KodiTvShowsScreen::RebuildEpisodeList(const std::vector<KodiEpisode>& episodes, bool truncated) {
    episodes_ = episodes;
    episodes_list_->SetItems(
        episodes_.size(),
        [this](size_t row) { return std::to_string(episodes_[row].episode) + ". " + episodes_[row].title; },
        [this](size_t row) { ShowEpisodeDetail(episodes_[row].episodeid); });
    episodes_list_->SetTruncated(truncated);
}

void KodiTvShowsScreen::ShowShowList() {
    lv_obj_set_hidden(seasons_container_, true);
    if (kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(shows_container_, false);
        shows_list_->Refresh();
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
    seasons_list_->Clear();
    seasons_.clear();

    lv_obj_set_hidden(shows_container_, true);
    lv_obj_set_hidden(seasons_container_, false);
    kodi_client_.RequestSeasons(tvshowid);
}

void KodiTvShowsScreen::ShowEpisodeList(int season) {
    selected_season_ = season;
    lv_label_set_text(episodes_title_label_, SeasonHeading(selected_show_title_, season).c_str());
    episodes_list_->Clear();
    episodes_.clear();

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

void KodiTvShowsScreen::OnPlayClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("episodeid", self->selected_episode_id_, /*resume=*/false);
    self->navigation_.GoTo(routes::kKodiNowPlaying);
}

void KodiTvShowsScreen::OnResumeClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    self->kodi_client_.OpenLibraryItem("episodeid", self->selected_episode_id_, /*resume=*/true);
    self->navigation_.GoTo(routes::kKodiNowPlaying);
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
        self->seasons_list_->Refresh();
    }
}

void KodiTvShowsScreen::OnEpisodeDetailBackClicked(lv_event_t* e) {
    auto* self = static_cast<KodiTvShowsScreen*>(lv_event_get_user_data(e));
    lv_obj_set_hidden(self->episode_detail_container_, true);
    if (self->kodi_client_.Snapshot().state == KodiConnectionState::kConnected) {
        lv_obj_set_hidden(self->episodes_container_, false);
        self->episodes_list_->Refresh();
    }
}

}  // namespace homedeck
