#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"

namespace homedeck {

// The Kodi module's Now Playing / transport screen, reached by tapping
// KodiWidget on the dashboard. Fully push-driven: every element
// re-renders from KodiClient::Snapshot() on
// KodiConnectionStateChangedEvent / KodiNowPlayingChangedEvent, with no
// optimistic local state (unlike ActivitiesScreen) - Kodi reports the
// resulting state itself within a pump cycle, so there is nothing to
// bridge.
class NowPlayingScreen {
public:
    NowPlayingScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                     KodiClient& kodi_client, Navigation& navigation);
    // root_ has no owning parent - deleting it recursively deletes
    // status_bar_'s objects too (see ui.md#object-lifecycle).
    ~NowPlayingScreen();

    NowPlayingScreen(const NowPlayingScreen&) = delete;
    NowPlayingScreen& operator=(const NowPlayingScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    enum class Action { kPlayPause, kStop, kSeekBack, kSeekForward, kVolumeDown, kVolumeUp, kMute, kOpenRemote };

    void Refresh();
    static void OnActionClicked(lv_event_t* e);

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;         // shown instead of content_ when not connected
    lv_obj_t* content_;           // holds everything below
    lv_obj_t* subtitle_label_;
    lv_obj_t* progress_bar_;
    lv_obj_t* time_label_;
    lv_obj_t* play_pause_label_;  // flips between the PLAY and PAUSE glyph on Refresh()
    lv_obj_t* rewind_button_;     // disabled on Refresh() when !now_playing.can_seek
    lv_obj_t* ff_button_;         // same

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription now_playing_sub_;
};

}  // namespace homedeck
