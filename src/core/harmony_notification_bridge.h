#pragma once

#include "core/event_bus.h"
#include "core/harmony_connection.h"

namespace homedeck {

// Publishes a NotificationEvent once when HarmonyConnection enters
// kError (a connection attempt failed), not on every retry while it
// stays there - same latching reasoning as LowBatteryMonitor's own
// NotificationEvent, needed here since HarmonyConnection's own
// retry/backoff loop re-enters kError on every failed attempt during a
// sustained outage. The latch resets on kConnected (an actual recovery),
// so a later, separate failure notifies again - not on kDisconnected/
// kConnecting, which are just points along the same still-failing retry
// cycle, not a recovery.
//
// Only needs EventBus& - HarmonyConnectionStateChangedEvent already
// carries the new state directly, so there's nothing to read back from
// HarmonyConnection itself (unlike LowBatteryMonitor/NetworkStatusMonitor,
// which poll a plain reader on Clock's tick because the thing they watch
// has no event of its own).
class HarmonyNotificationBridge {
public:
    explicit HarmonyNotificationBridge(EventBus& event_bus);

private:
    bool already_notified_ = false;
    EventBus::ScopedSubscription state_subscription_;
};

}  // namespace homedeck
