#include "ui/screens/now_playing_screen.h"

#include "ui/kodi_display.h"
#include "ui/remote_button.h"
#include "ui/screens/screen_chrome.h"

#include <algorithm>
#include <cstdint>

namespace homedeck {

namespace {

constexpr int kSeekStepPercent = 5;
constexpr int kVolumeStep = 5;
constexpr int32_t kBarRange = 1000;

lv_obj_t* CreateRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    return row;
}

}  // namespace

NowPlayingScreen::NowPlayingScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                                  KodiClient& kodi_client, Navigation& navigation)
    : kodi_client_(kodi_client),
      navigation_(navigation),
      root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status) {
    ScreenChrome chrome = CreateScreenChrome(root_, "Now Playing", "Kodi not connected.", navigation);
    hint_label_ = chrome.hint_label;
    content_ = chrome.content_container;
    lv_obj_t* home_button = chrome.home_button;

    subtitle_label_ = lv_label_create(content_);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle_label_, LV_PCT(100));
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);

    progress_bar_ = lv_bar_create(content_);
    lv_obj_set_size(progress_bar_, LV_PCT(100), 12);
    lv_bar_set_range(progress_bar_, 0, kBarRange);
    lv_bar_set_value(progress_bar_, 0, LV_ANIM_OFF);

    time_label_ = lv_label_create(content_);
    lv_label_set_text(time_label_, "0:00 / 0:00");

    auto add_button = [this](lv_obj_t* row, const char* label, Action action, int32_t width) {
        lv_obj_t* button = CreateRemoteButton(row, label, width);
        lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(action)));
        lv_obj_add_event_cb(button, OnActionClicked, LV_EVENT_CLICKED, this);
        return button;
    };

    // The two outer buttons are a coarse rewind / fast-forward (a
    // percentage seek), drawn as a double-triangle by SetTransportGlyph
    // (remote_button.h). LV_SYMBOL_PREV/NEXT stay reserved for genuine
    // skip-track (DevicesScreen maps SkipBackward/SkipForward to them).
    lv_obj_t* transport_row = CreateRow(content_);

    lv_obj_t* rewind_button = add_button(transport_row, "", Action::kSeekBack, LV_PCT(23));
    SetTransportGlyph(rewind_button, /*pointing_left=*/true);

    lv_obj_t* play_pause_button = add_button(transport_row, LV_SYMBOL_PLAY, Action::kPlayPause, LV_PCT(23));
    play_pause_label_ = lv_obj_get_child(play_pause_button, 0);
    add_button(transport_row, LV_SYMBOL_STOP, Action::kStop, LV_PCT(23));

    lv_obj_t* ff_button = add_button(transport_row, "", Action::kSeekForward, LV_PCT(23));
    SetTransportGlyph(ff_button, /*pointing_left=*/false);

    // Mute, then down, then up - matching how Harmony's generic command
    // grid orders Mute / VolumeDown / VolumeUp (and PrevChannel /
    // ChannelDown / ChannelUp): the toggle first, then decrease, then
    // increase.
    lv_obj_t* volume_row = CreateRow(content_);
    add_button(volume_row, LV_SYMBOL_MUTE, Action::kMute, LV_PCT(31));
    add_button(volume_row, LV_SYMBOL_MINUS, Action::kVolumeDown, LV_PCT(31));
    add_button(volume_row, LV_SYMBOL_PLUS, Action::kVolumeUp, LV_PCT(31));

    add_button(content_, "Remote", Action::kOpenRemote, LV_PCT(100));

    Refresh();

    state_sub_ = event_bus.SubscribeUi<KodiConnectionStateChangedEvent>(
        [this](const KodiConnectionStateChangedEvent&) { Refresh(); });
    now_playing_sub_ =
        event_bus.SubscribeUi<KodiNowPlayingChangedEvent>([this](const KodiNowPlayingChangedEvent&) { Refresh(); });

    lv_obj_move_foreground(status_bar_.Root());
    lv_obj_move_foreground(home_button);
}

NowPlayingScreen::~NowPlayingScreen() {
    // Ahead of member destruction - each callback reads `this`'s members
    // (same reasoning as ActivitiesScreen's destructor).
    state_sub_.Reset();
    now_playing_sub_.Reset();
    lv_obj_del(root_);
}

void NowPlayingScreen::Refresh() {
    KodiSnapshot snapshot = kodi_client_.Snapshot();
    const bool connected = snapshot.state == KodiConnectionState::kConnected;

    if (connected) {
        lv_obj_clear_flag(content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(hint_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const KodiNowPlaying& np = snapshot.now_playing;
    lv_label_set_text(subtitle_label_, KodiNowPlayingSubtitle(np).c_str());

    // Explicit <int32_t> - esp-idf's int32_t is `long`, so a bare
    // std::clamp(long, int, long) fails template deduction on firmware
    // even though it's fine on the host.
    int32_t bar_value =
        np.duration_ms > 0
            ? std::clamp<int32_t>(static_cast<int32_t>(np.percent * (kBarRange / 100.0)), 0, kBarRange)
            : 0;
    lv_bar_set_value(progress_bar_, bar_value, LV_ANIM_OFF);

    lv_label_set_text(time_label_,
                      (FormatKodiClock(np.position_ms) + " / " + FormatKodiClock(np.duration_ms)).c_str());

    lv_label_set_text(play_pause_label_,
                      np.playback == KodiPlaybackState::kPlaying ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void NowPlayingScreen::OnActionClicked(lv_event_t* e) {
    auto* self = static_cast<NowPlayingScreen*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto action = static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));

    KodiSnapshot snapshot = self->kodi_client_.Snapshot();
    switch (action) {
        case Action::kPlayPause:
            self->kodi_client_.PlayPause();
            break;
        case Action::kStop:
            self->kodi_client_.StopPlayback();
            break;
        case Action::kSeekBack:
            self->kodi_client_.SeekPercent(std::clamp(snapshot.now_playing.percent - kSeekStepPercent, 0.0, 100.0));
            break;
        case Action::kSeekForward:
            self->kodi_client_.SeekPercent(std::clamp(snapshot.now_playing.percent + kSeekStepPercent, 0.0, 100.0));
            break;
        case Action::kVolumeDown:
            self->kodi_client_.SetVolume(std::clamp(snapshot.volume - kVolumeStep, 0, 100));
            break;
        case Action::kVolumeUp:
            self->kodi_client_.SetVolume(std::clamp(snapshot.volume + kVolumeStep, 0, 100));
            break;
        case Action::kMute:
            self->kodi_client_.ToggleMute();
            break;
        case Action::kOpenRemote:
            self->navigation_.GoTo("kodi-remote");
            break;
    }
}

}  // namespace homedeck
