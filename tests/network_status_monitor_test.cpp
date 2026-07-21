#include "core/network_status_monitor.h"

#include "platform/host/network_status.h"

#include <gtest/gtest.h>

TEST(NetworkStatusMonitor, DoesNotPublishWhileConnectedStateStaysTheSame) {
    homedeck::EventBus bus;
    homedeck::HostNetworkStatus network_status;  // defaults to connected
    homedeck::NetworkStatusMonitor monitor(bus, network_status);

    int events = 0;
    auto sub = bus.Subscribe<homedeck::WifiConnectivityChangedEvent>(
        [&events](const homedeck::WifiConnectivityChangedEvent&) { events++; });

    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});

    // Latched - three ticks while still connected must not publish
    // three times.
    EXPECT_EQ(events, 0);
}

TEST(NetworkStatusMonitor, PublishesOnceWhenTransitioningToDisconnected) {
    homedeck::EventBus bus;
    homedeck::HostNetworkStatus network_status;
    homedeck::NetworkStatusMonitor monitor(bus, network_status);

    int events = 0;
    bool last_connected = true;
    auto sub = bus.Subscribe<homedeck::WifiConnectivityChangedEvent>(
        [&events, &last_connected](const homedeck::WifiConnectivityChangedEvent& event) {
            events++;
            last_connected = event.connected;
        });

    network_status.SetConnected(false);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(events, 1);
    EXPECT_FALSE(last_connected);
}

TEST(NetworkStatusMonitor, PublishesAgainAfterReconnecting) {
    homedeck::EventBus bus;
    homedeck::HostNetworkStatus network_status;
    homedeck::NetworkStatusMonitor monitor(bus, network_status);

    int events = 0;
    auto sub = bus.Subscribe<homedeck::WifiConnectivityChangedEvent>(
        [&events](const homedeck::WifiConnectivityChangedEvent&) { events++; });

    network_status.SetConnected(false);
    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(events, 1);

    network_status.SetConnected(true);
    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(events, 2);
}
