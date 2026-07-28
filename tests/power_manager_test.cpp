#include "core/power_manager.h"

#include "core/clock.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

class FakeUserActivitySource : public homedeck::UserActivitySource {
public:
    void SetMs(uint32_t ms) { ms_ = ms; }
    uint32_t MillisecondsSinceLastActivity() const override { return ms_; }

private:
    uint32_t ms_ = 0;
};

class FakeDisplayBrightness : public homedeck::DisplayBrightness {
public:
    void SetPercent(int percent) override {
        last_percent = percent;
        call_count++;
    }

    int last_percent = -1;
    int call_count = 0;
};

class FakeTimeSource : public homedeck::TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override { return fixed_time; }

    std::chrono::system_clock::time_point fixed_time =
        std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
};

// SubscribeUi silently drops delivery with no dispatcher registered
// (see event_bus.cpp) - PowerManager subscribes via SubscribeUi (it
// touches LVGL indirectly through both interfaces above, unlike
// LowBatteryMonitor/NetworkStatusMonitor), so every test needs this,
// same idiom tests/event_bus_test.cpp already uses.
void RunDispatcherInline(homedeck::EventBus& bus) {
    bus.SetUiDispatcher([](std::function<void()> fn) { fn(); });
}

}  // namespace

TEST(PowerManager, StartsActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
}

TEST(PowerManager, StaysActiveWhileRecentlyActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.call_count, 0);
}

TEST(PowerManager, TransitionsToIdleOnceInactiveLongEnough) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    // An extreme value, not the real threshold constant - this test
    // must survive the placeholder timeout being retuned later.
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
    EXPECT_EQ(brightness.call_count, 1);
    EXPECT_LT(brightness.last_percent, 100);
}

TEST(PowerManager, IdleTransitionIsLatchedNotRepeatedEveryTick) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});

    // Still idle, but SetPercent() shouldn't fire again for ticks 2/3 -
    // same "latched, not per-tick" reasoning as LowBatteryMonitor.
    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
    EXPECT_EQ(brightness.call_count, 1);
}

TEST(PowerManager, ReturnsToActiveOnRealActivity) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kIdle);

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.call_count, 2);
    EXPECT_EQ(brightness.last_percent, 100);
}

TEST(PowerManager, PublishesPowerStateChangedEventOnEachRealTransition) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});  // still idle - must not publish again

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0], homedeck::PowerState::kIdle);
    EXPECT_EQ(events[1], homedeck::PowerState::kActive);
}

TEST(PowerManager, SleepVetoIsActiveUntilItExpires) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    EXPECT_FALSE(manager.HasActiveSleepVeto());

    manager.RequestSleepVeto(std::chrono::minutes(5));
    EXPECT_TRUE(manager.HasActiveSleepVeto());

    time_source.fixed_time += std::chrono::minutes(4);
    EXPECT_TRUE(manager.HasActiveSleepVeto());

    time_source.fixed_time += std::chrono::minutes(2);
    EXPECT_FALSE(manager.HasActiveSleepVeto());
}

TEST(PowerManager, RepeatedSleepVetoRequestsOverwriteRatherThanStack) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    manager.RequestSleepVeto(std::chrono::minutes(10));
    manager.RequestSleepVeto(std::chrono::minutes(1));  // overwrites, doesn't extend

    time_source.fixed_time += std::chrono::minutes(2);
    EXPECT_FALSE(manager.HasActiveSleepVeto());
}

TEST(PowerManager, ActiveSleepVetoDoesNotBlockIdleTransitionThisPhase) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source);

    manager.RequestSleepVeto(std::chrono::minutes(30));
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    // The veto exists and is tested above, but Sleeping is unreachable
    // this phase, so it has nothing to veto yet - this asserts that
    // real, current behavior explicitly rather than leaving it implicit.
    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
}
