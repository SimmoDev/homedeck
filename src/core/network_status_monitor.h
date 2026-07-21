#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/network_status.h"

namespace homedeck {

// Published only on an actual connected/disconnected transition, not on
// every tick while the state holds - same latching reasoning as
// LowBatteryMonitor's NotificationEvent.
struct WifiConnectivityChangedEvent {
    bool connected;
};

// Structural twin of LowBatteryMonitor: polls a portable NetworkStatus
// on Clock's existing tick and publishes WifiConnectivityChangedEvent
// only on a real transition. This layer - not NetworkStatus itself -
// owns the EventBus dependency, so the same portable, testable path
// backs both targets (FirmwareNetworkStatus's actual firmware event
// handler and HostNetworkStatus's plain setters both flow through this
// one class rather than each needing their own publish logic).
class NetworkStatusMonitor {
public:
    NetworkStatusMonitor(EventBus& event_bus, NetworkStatus& network_status);

private:
    NetworkStatus& network_status_;
    // Seeded from the real state at construction (see .cpp), not a
    // hardcoded false - otherwise the first tick would always look like
    // a transition merely because the sentinel doesn't match reality,
    // in the common case where the device is already connected.
    bool last_connected_;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
