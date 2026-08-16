#include "core/harmony_notification_bridge.h"

#include "core/notification.h"

namespace homedeck {

HarmonyNotificationBridge::HarmonyNotificationBridge(EventBus& event_bus) {
    state_subscription_ = event_bus.Subscribe<HarmonyConnectionStateChangedEvent>(
        [this, &event_bus](const HarmonyConnectionStateChangedEvent& event) {
            if (event.state == HarmonyConnectionState::kError) {
                if (!already_notified_) {
                    already_notified_ = true;
                    event_bus.Publish(NotificationEvent{"Harmony Hub connection lost", NotificationSeverity::kDeferred});
                }
            } else if (event.state == HarmonyConnectionState::kConnected) {
                already_notified_ = false;
            }
        });
}

}  // namespace homedeck
