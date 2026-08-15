#include "ui/harmony_widget.h"

#include "ui/theme.h"
#include "ui/widget_tile.h"

#include <string>

namespace homedeck {

namespace {

std::string StatusLine(const HarmonyConnectionSnapshot& snapshot) {
    switch (snapshot.state) {
        case HarmonyConnectionState::kDisconnected:
            return "Not connected";
        case HarmonyConnectionState::kConnecting:
            return "Connecting...";
        case HarmonyConnectionState::kError:
            return "Connection error";
        case HarmonyConnectionState::kConnected:
            break;
    }
    if (!snapshot.has_config) {
        return "Connected";
    }
    for (const HarmonyActivity& activity : snapshot.activities) {
        if (activity.id == snapshot.current_activity_id) {
            return activity.label;
        }
    }
    // Connected, has a config, but current_activity_id is either still
    // empty (first fetch hasn't landed yet) or doesn't match anything in
    // `activities` (shouldn't happen - defensive, not a real case seen).
    return "Connected";
}

}  // namespace

HarmonyWidget::HarmonyWidget(lv_obj_t* parent, EventBus& event_bus, HarmonyConnection& harmony_connection,
                              Navigation& navigation)
    : harmony_connection_(harmony_connection), navigation_(navigation) {
    root_ = CreateWidgetTileRoot(parent);

    lv_obj_t* title = lv_label_create(root_);
    lv_obj_set_style_text_font(title, kBodyFont, 0);
    lv_label_set_text(title, "Harmony");

    label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(label_, kBodyFont, 0);

    Refresh();

    state_sub_ = event_bus.SubscribeUi<HarmonyConnectionStateChangedEvent>(
        [this](const HarmonyConnectionStateChangedEvent&) { Refresh(); });
    config_sub_ =
        event_bus.SubscribeUi<HarmonyConfigUpdatedEvent>([this](const HarmonyConfigUpdatedEvent&) { Refresh(); });
    activity_sub_ = event_bus.SubscribeUi<HarmonyCurrentActivityChangedEvent>(
        [this](const HarmonyCurrentActivityChangedEvent&) { Refresh(); });
}

void HarmonyWidget::Refresh() { lv_label_set_text(label_, StatusLine(harmony_connection_.Snapshot()).c_str()); }

void HarmonyWidget::OnTap() { navigation_.GoTo("harmony-activities"); }

}  // namespace homedeck
