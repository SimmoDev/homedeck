#pragma once

#include "core/event_bus.h"
#include "core/kodi_client.h"
#include "lvgl.h"
#include "ui/navigation.h"
#include "ui/widget.h"

namespace homedeck {

// What Kodi is playing (Kodi module) - the dashboard's second module
// widget, following HarmonyWidget's shape exactly. Tapping it opens
// NowPlayingScreen (src/ui/screens/now_playing_screen.h).
class KodiWidget : public Widget {
public:
    KodiWidget(lv_obj_t* parent, EventBus& event_bus, KodiClient& kodi_client, Navigation& navigation);

    lv_obj_t* Root() const override { return root_; }
    // Same reasoning as HarmonyWidget - a title (a show name / "not
    // reachable") truncates too easily in a 1x1 cell.
    int ColumnSpan() const override { return 2; }

    void OnTap() override;

private:
    void Refresh();

    KodiClient& kodi_client_;
    Navigation& navigation_;

    lv_obj_t* root_;
    lv_obj_t* label_;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription now_playing_sub_;
};

}  // namespace homedeck
