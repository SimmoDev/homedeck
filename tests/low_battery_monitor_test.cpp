#include "core/low_battery_monitor.h"

#include "core/notification.h"

#include <gtest/gtest.h>

namespace {

class FakeBatteryReader : public homedeck::BatteryReader {
public:
    void SetPercent(int percent) { percent_ = percent; }
    int ReadPercent() const override { return percent_; }
    bool IsExternalPowerConnected() const override { return false; }
    bool IsBatteryPresent() const override { return present_; }
    void SetBatteryPresent(bool present) { present_ = present; }

private:
    int percent_ = 100;
    bool present_ = true;
};

}  // namespace

TEST(LowBatteryMonitor, DoesNotNotifyAboveThreshold) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(50);
    homedeck::LowBatteryMonitor monitor(bus, battery);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(notifications, 0);
}

TEST(LowBatteryMonitor, NotifiesOnceWhenCrossingBelowThreshold) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(50);
    homedeck::LowBatteryMonitor monitor(bus, battery);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    battery.SetPercent(10);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});

    // Latched - three ticks while still low must only notify once, not
    // once per tick.
    EXPECT_EQ(notifications, 1);
}

TEST(LowBatteryMonitor, NotifiesAgainAfterRecoveringThenDroppingLowAgain) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(10);
    homedeck::LowBatteryMonitor monitor(bus, battery);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(notifications, 1);

    battery.SetPercent(80);  // recharged
    bus.Publish(homedeck::ClockTickEvent{});

    battery.SetPercent(10);  // low again - a real second episode
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(notifications, 2);
}

TEST(LowBatteryMonitor, DoesNotNotifyWhenNoBatteryPresentEvenAtZeroPercent) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(0);
    battery.SetBatteryPresent(false);
    homedeck::LowBatteryMonitor monitor(bus, battery);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(notifications, 0);
}

TEST(LowBatteryMonitor, StillNotifiesOnceBatteryIsInsertedAndGenuinelyLow) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(0);
    battery.SetBatteryPresent(false);
    homedeck::LowBatteryMonitor monitor(bus, battery);

    int notifications = 0;
    auto sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent&) { notifications++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(notifications, 0);

    // A battery gets inserted, already low - the earlier "absent" ticks
    // must not have latched the notified flag shut.
    battery.SetBatteryPresent(true);
    battery.SetPercent(10);
    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(notifications, 1);
}
