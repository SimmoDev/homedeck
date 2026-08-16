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
