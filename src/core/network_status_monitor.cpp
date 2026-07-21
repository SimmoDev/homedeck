#include "core/network_status_monitor.h"

namespace homedeck {

NetworkStatusMonitor::NetworkStatusMonitor(EventBus& event_bus, NetworkStatus& network_status)
    : network_status_(network_status), last_connected_(network_status.Snapshot().connected) {
    clock_subscription_ = event_bus.Subscribe<ClockTickEvent>([this, &event_bus](const ClockTickEvent&) {
        bool connected = network_status_.Snapshot().connected;
        if (connected != last_connected_) {
            last_connected_ = connected;
            event_bus.Publish(WifiConnectivityChangedEvent{connected});
        }
    });
}

}  // namespace homedeck
