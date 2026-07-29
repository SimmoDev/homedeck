#pragma once

#include "core/event_bus.h"
#include "core/notification_sound.h"
#include "core/power_manager.h"
#include "core/storage.h"
#include "lvgl.h"

namespace homedeck {

// Published by StatusBar's settings icon tap - see status_bar.cpp. Empty:
// there's exactly one thing this can mean (open the panel), no payload
// needed.
struct QuickSettingsRequestedEvent {};

// A small top-layer overlay (same lv_layer_top() shape as
// NotificationBanner - see notification_banner.h) exposing the two
// hardware controls that already exist but have no user-facing control
// yet: DisplayBrightness::SetPercent() (via PowerManager, which owns
// applying it - see power_manager.h) and AudioOutput::SetVolume() (via
// NotificationSound, the only current audio consumer). A single instance
// covers every screen, the same reason NotificationBanner is a single
// instance - reachable regardless of which screen Navigation has loaded,
// not just from the dashboard, since volume/brightness are as likely to
// need adjusting while looking at some other screen as at the dashboard.
class QuickSettingsPanel {
public:
    QuickSettingsPanel(EventBus& event_bus, PowerManager& power_manager,
                        NotificationSound& notification_sound, Storage& storage);
    // scrim_/panel_ have no owning parent (see ui.md#object-lifecycle) -
    // both are direct, sibling children of lv_layer_top(), so deleting
    // one doesn't delete the other; both need their own call.
    ~QuickSettingsPanel();

    QuickSettingsPanel(const QuickSettingsPanel&) = delete;
    QuickSettingsPanel& operator=(const QuickSettingsPanel&) = delete;

    // Called from the scrim's click callback (a free function, not a
    // member - matches home_affordance.cpp's existing LVGL event-bridging
    // idiom), so it needs to be public rather than reached through a
    // friend declaration.
    void Hide();

private:
    void Show();

    PowerManager& power_manager_;
    NotificationSound& notification_sound_;
    Storage& storage_;

    // Full-screen, invisible, sits behind panel_ - its click handler
    // hides both. Tapping the status bar's settings icon again while
    // already open hits this (it's above the icon in z-order, both being
    // on lv_layer_top() while StatusBar is not), so no separate
    // toggle-to-close handling is needed on the icon itself.
    lv_obj_t* scrim_;
    lv_obj_t* panel_;
    lv_obj_t* brightness_slider_;
    lv_obj_t* volume_slider_;

    EventBus::ScopedSubscription subscription_;
};

}  // namespace homedeck
