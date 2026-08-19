#include "core/harmony_notification_bridge.h"

#include "core/notification.h"

#include <gtest/gtest.h>

TEST(HarmonyNotificationBridge, DoesNotNotifyOnConnectingOrDisconnected) {
    homedeck::EventBus bus;
    homedeck::HarmonyNotificationBridge bridge(bus);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kDisconnected});
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kConnecting});
    EXPECT_EQ(notifications, 0);
}

TEST(HarmonyNotificationBridge, NotifiesOnceWhenEnteringError) {
    homedeck::EventBus bus;
    homedeck::HarmonyNotificationBridge bridge(bus);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});
    // A retry attempt cycling back through kConnecting/kError while still
    // failing must not notify again - latched the same way
    // LowBatteryMonitor's own NotificationEvent is.
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kConnecting});
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});

    EXPECT_EQ(notifications, 1);
}

TEST(HarmonyNotificationBridge, NotifiesAgainAfterReconfiguringToADifferentHubThatAlsoFails) {
    homedeck::EventBus bus;
    homedeck::HarmonyNotificationBridge bridge(bus);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    // Hub A fails.
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});
    EXPECT_EQ(notifications, 1);

    // The user re-points hub_host at a different address (B), never
    // reaching kConnected in between - HarmonyConnection publishes
    // HarmonyConfigUpdatedEvent for this (ClearConfigIfPresent()'s
    // force_publish), then retries via kConnecting/kError as usual.
    bus.Publish(homedeck::HarmonyConfigUpdatedEvent{});
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kConnecting});
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});

    // Hub B's own failure must notify too, not be swallowed by hub A's
    // still-latched notification.
    EXPECT_EQ(notifications, 2);
}

TEST(HarmonyNotificationBridge, NotifiesAgainAfterRecoveringThenFailingAgain) {
    homedeck::EventBus bus;
    homedeck::HarmonyNotificationBridge bridge(bus);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});
    EXPECT_EQ(notifications, 1);

    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kConnected});
    bus.Publish(homedeck::HarmonyConnectionStateChangedEvent{homedeck::HarmonyConnectionState::kError});

    EXPECT_EQ(notifications, 2);
}
