#include "core/critical_battery_monitor.h"

#include "core/notification.h"

#include <gtest/gtest.h>

#include <vector>

namespace {

class FakeBatteryReader : public homedeck::BatteryReader {
public:
    void SetPercent(int percent) { percent_ = percent; }
    int ReadPercent() const override { return percent_; }
    bool IsExternalPowerConnected() const override { return external_power_connected_; }
    void SetExternalPowerConnected(bool connected) { external_power_connected_ = connected; }
    bool IsBatteryPresent() const override { return present_; }
    void SetBatteryPresent(bool present) { present_ = present; }

private:
    int percent_ = 100;
    bool external_power_connected_ = false;
    bool present_ = true;
};

}  // namespace

TEST(CriticalBatteryMonitor, DoesNotTriggerAboveThreshold) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(50);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    int state_events = 0;
    auto sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent&) { state_events++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(state_events, 0);
}

TEST(CriticalBatteryMonitor, TriggersOnceWhenCrossingBelowThreshold) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(50);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    std::vector<bool> state_events;
    auto state_sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent& event) {
            state_events.push_back(event.critical);
        });
    std::vector<homedeck::NotificationSeverity> notifications;
    auto notification_sub = bus.Subscribe<homedeck::NotificationEvent>(
        [&notifications](const homedeck::NotificationEvent& event) { notifications.push_back(event.severity); });

    battery.SetPercent(2);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});

    // Latched - three ticks while still critical must only trigger once,
    // not once per tick, same reasoning LowBatteryMonitor's own latch
    // test asserts.
    ASSERT_EQ(state_events.size(), 1u);
    EXPECT_TRUE(state_events[0]);
    ASSERT_EQ(notifications.size(), 1u);
    EXPECT_EQ(notifications[0], homedeck::NotificationSeverity::kAlertPriority);
}

TEST(CriticalBatteryMonitor, ClearsAndCanTriggerAgainAfterRecoveringThenDroppingLowAgain) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(2);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    std::vector<bool> state_events;
    auto sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent& event) {
            state_events.push_back(event.critical);
        });

    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(state_events.size(), 1u);
    EXPECT_TRUE(state_events[0]);

    battery.SetPercent(80);  // recovered (e.g. plugged in and charged)
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(state_events.size(), 2u);
    EXPECT_FALSE(state_events[1]);

    battery.SetPercent(2);  // critical again - a real second episode
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(state_events.size(), 3u);
    EXPECT_TRUE(state_events[2]);
}

TEST(CriticalBatteryMonitor, DoesNotTriggerWhenNoBatteryPresentEvenAtZeroPercent) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(0);
    battery.SetBatteryPresent(false);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    int state_events = 0;
    auto sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent&) { state_events++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(state_events, 0);
}

TEST(CriticalBatteryMonitor, StillTriggersOnceBatteryIsInsertedAndGenuinelyLow) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(0);
    battery.SetBatteryPresent(false);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    int state_events = 0;
    auto sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent&) { state_events++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(state_events, 0);

    // A battery gets inserted, already critical - the earlier "absent"
    // ticks must not have latched the already_critical_ flag shut.
    battery.SetBatteryPresent(true);
    battery.SetPercent(2);
    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(state_events, 1);
}

TEST(CriticalBatteryMonitor, DoesNotTriggerWhenExternalPowerConnectedEvenBelowThreshold) {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    battery.SetPercent(2);
    battery.SetExternalPowerConnected(true);
    homedeck::CriticalBatteryMonitor monitor(bus, battery);

    int state_events = 0;
    auto sub = bus.Subscribe<homedeck::CriticalBatteryStateChangedEvent>(
        [&state_events](const homedeck::CriticalBatteryStateChangedEvent&) { state_events++; });

    bus.Publish(homedeck::ClockTickEvent{});
    EXPECT_EQ(state_events, 0);
}
