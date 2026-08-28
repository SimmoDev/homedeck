#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"

namespace homedeck {

// The Kodi module's UI-navigation remote - a D-pad plus Back / Home /
// Info / OSD / Context Menu. Reached from NowPlayingScreen's "Remote"
// button. Every button issues one KodiClient::SendInput() on a plain
// tap - no press/hold/release, which is why this builds its own small
// cross rather than sharing DevicesScreen's hold-capable D-pad code.
class KodiRemoteScreen {
public:
    KodiRemoteScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                     KodiClient& kodi_client, Navigation& navigation);
    ~KodiRemoteScreen();

    KodiRemoteScreen(const KodiRemoteScreen&) = delete;
    KodiRemoteScreen& operator=(const KodiRemoteScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

private:
    void Refresh();
    static void OnInputClicked(lv_event_t* e);

    KodiClient& kodi_client_;

    lv_obj_t* root_;
    StatusBar status_bar_;
    lv_obj_t* hint_label_;  // shown instead of content_ when not connected
    lv_obj_t* content_;

    EventBus::ScopedSubscription state_sub_;
};

}  // namespace homedeck
