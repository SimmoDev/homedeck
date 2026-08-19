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
// Also resets on HarmonyConfigUpdatedEvent - published (among other
// things) whenever ConnectionLoop() starts trying a *different*
// hub_host (see ClearConfigIfPresent()'s force_publish parameter), which
// is not itself a recovery but does mean the very next kError is a fresh
// address's own first failure, not a continuation of the previous
// address's already-notified one; without this, editing hub_host from
// one unreachable address to another would silently swallow the second
// address's failure notification, since neither transition passes
// through kConnected.
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
    EventBus::ScopedSubscription config_subscription_;
};

}  // namespace homedeck
