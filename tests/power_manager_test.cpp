#include "core/power_manager.h"

#include "core/clock.h"
#include "core/critical_battery_monitor.h"
#include "core/ota_routes.h"

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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
}

TEST(PowerManager, StaysActiveWhileRecentlyActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    // Past kIdleTimeoutMs but short of kSleepTimeoutMs - UINT32_MAX would
    // also cross into Sleeping territory on a later tick, which is a
    // different behavior this test isn't about.
    activity.SetMs(60000);
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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    // Past kIdleTimeoutMs but short of kSleepTimeoutMs - see the latch
    // test above for why this can't be UINT32_MAX here.
    activity.SetMs(60000);
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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

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
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.RequestSleepVeto(std::chrono::minutes(10));
    manager.RequestSleepVeto(std::chrono::minutes(1));  // overwrites, doesn't extend

    time_source.fixed_time += std::chrono::minutes(2);
    EXPECT_FALSE(manager.HasActiveSleepVeto());
}

TEST(PowerManager, ActiveSleepVetoDoesNotBlockActiveToIdleTransition) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.RequestSleepVeto(std::chrono::minutes(30));
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    // The veto only ever gates entry into Sleeping (see
    // SleepVetoBlocksIdleToSleepingTransition below) - it must not also
    // block the unrelated Active->Idle transition.
    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
}

TEST(PowerManager, TransitionsToSleepingAfterExtendedInactivity) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});  // Active -> Idle
    bus.Publish(homedeck::ClockTickEvent{});  // Idle -> Sleeping

    EXPECT_EQ(manager.State(), homedeck::PowerState::kSleeping);
    EXPECT_EQ(brightness.last_percent, 0);
}

TEST(PowerManager, WakesFromSleepingDirectlyToActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kSleeping);

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.last_percent, 100);
}

TEST(PowerManager, PublishesPowerStateChangedEventOnSleepingTransition) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});  // Active -> Idle
    bus.Publish(homedeck::ClockTickEvent{});  // Idle -> Sleeping

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});  // Sleeping -> Active

    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0], homedeck::PowerState::kIdle);
    EXPECT_EQ(events[1], homedeck::PowerState::kSleeping);
    EXPECT_EQ(events[2], homedeck::PowerState::kActive);
}

TEST(PowerManager, SleepVetoBlocksIdleToSleepingTransition) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.RequestSleepVeto(std::chrono::minutes(30));
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});  // Active -> Idle, unaffected by the veto
    ASSERT_EQ(manager.State(), homedeck::PowerState::kIdle);

    bus.Publish(homedeck::ClockTickEvent{});  // Would reach Sleeping without the veto

    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
}

TEST(PowerManager, SleepVetoExpiryAllowsSleepingOnLaterTick) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.RequestSleepVeto(std::chrono::minutes(1));
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});  // Active -> Idle
    bus.Publish(homedeck::ClockTickEvent{});  // Veto still active - stays Idle
    ASSERT_EQ(manager.State(), homedeck::PowerState::kIdle);

    time_source.fixed_time += std::chrono::minutes(2);
    bus.Publish(homedeck::ClockTickEvent{});  // Veto expired - proceeds to Sleeping

    EXPECT_EQ(manager.State(), homedeck::PowerState::kSleeping);
}

TEST(PowerManager, OtaInProgressTransitionsToUpdating) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kUpdating);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], homedeck::PowerState::kUpdating);
}

TEST(PowerManager, IdleTimeoutDoesNotFireWhileUpdating) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kUpdating);

    // Would trigger kIdle on any other tick (see
    // TransitionsToIdleOnceInactiveLongEnough) - must not while Updating.
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kUpdating);
}

TEST(PowerManager, OtaFinishedReturnsToActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kUpdating);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.last_percent, 100);
}

TEST(PowerManager, OtaFinishedLandsOnIdleIfInactivityAlreadyExceedsIdleTimeout) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kUpdating);

    // Past the idle threshold (accumulated during the write, since
    // OnTick()'s idle timeout is suppressed the whole time - see its own
    // comment) but not the sleep one.
    activity.SetMs(60000);
    int calls_before_finish = brightness.call_count;
    bus.Publish(homedeck::OtaUpdateStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
    // Exactly one more SetPercent call landing directly on Idle's
    // brightness - not a flash to Active's brightness followed
    // immediately by a second call back down once OnTick() next fires.
    EXPECT_EQ(brightness.call_count, calls_before_finish + 1);
}

TEST(PowerManager, OtaFinishedLandsOnSleepingIfInactivityAlreadyExceedsSleepTimeout) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kUpdating);

    activity.SetMs(UINT32_MAX);
    int calls_before_finish = brightness.call_count;
    bus.Publish(homedeck::OtaUpdateStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kSleeping);
    EXPECT_EQ(brightness.call_count, calls_before_finish + 1);
}

TEST(PowerManager, CriticalBatteryTransitionsToError) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{true});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kError);
    EXPECT_EQ(brightness.last_percent, 100);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], homedeck::PowerState::kError);
}

TEST(PowerManager, CriticalBatteryClearsBackToActiveOnRecovery) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kError);

    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.last_percent, 100);
}

TEST(PowerManager, ClearingCriticalBatteryWhileNotInErrorDoesNothing) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    std::vector<homedeck::PowerState> events;
    auto sub = bus.SubscribeUi<homedeck::PowerStateChangedEvent>(
        [&events](const homedeck::PowerStateChangedEvent& event) { events.push_back(event.state); });

    // Never entered kError - a stray {false} must not force a
    // transition (would otherwise still "work" here since kActive is
    // already the state, but this locks in the guard rather than
    // leaving it untested).
    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_TRUE(events.empty());
}

TEST(PowerManager, IdleTimeoutDoesNotFireWhileInError) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kError);

    // Would trigger kIdle on any other tick (see
    // TransitionsToIdleOnceInactiveLongEnough) - must not while in
    // Error. Leaving kError is exclusively the battery recovering.
    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kError);
}

TEST(PowerManager, CriticalBatteryInterruptsUpdating) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kUpdating);

    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{true});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kError);
}

TEST(PowerManager, OtaFinishingDoesNotClobberErrorState) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    bus.Publish(homedeck::OtaUpdateStateChangedEvent{true});
    bus.Publish(homedeck::CriticalBatteryStateChangedEvent{true});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kError);

    // The write finishing (success or failure - ota_routes.cpp publishes
    // {false} either way) must not silently override the still-critical
    // battery back to kActive.
    bus.Publish(homedeck::OtaUpdateStateChangedEvent{false});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kError);
}

TEST(PowerManager, InitialActiveBrightnessIsClamped) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    // Below the floor - see kMinActiveBrightnessPercent's own comment for
    // why 0 isn't allowed for a user-chosen Active brightness.
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 0);

    EXPECT_EQ(manager.ActiveBrightnessPercent(), 5);
}

TEST(PowerManager, SetActiveBrightnessPercentClampsToFloorAndCeiling) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.SetActiveBrightnessPercent(2);
    EXPECT_EQ(manager.ActiveBrightnessPercent(), 5);

    manager.SetActiveBrightnessPercent(150);
    EXPECT_EQ(manager.ActiveBrightnessPercent(), 100);
}

TEST(PowerManager, SetActiveBrightnessPercentAppliesImmediatelyWhileActive) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    manager.SetActiveBrightnessPercent(42);

    EXPECT_EQ(brightness.last_percent, 42);
}

TEST(PowerManager, SetActiveBrightnessPercentDoesNotApplyWhileIdle) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kIdle);
    int call_count_before = brightness.call_count;

    // Updates the stored baseline for next time Active, but must not
    // touch real hardware brightness while dimmed for Idle - only the
    // next Active transition should apply it (see the next test).
    manager.SetActiveBrightnessPercent(60);

    EXPECT_EQ(brightness.call_count, call_count_before);
    EXPECT_EQ(manager.ActiveBrightnessPercent(), 60);
}

TEST(PowerManager, NewActiveBrightnessBaselineAppliesOnNextActiveTransition) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 100);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});
    ASSERT_EQ(manager.State(), homedeck::PowerState::kIdle);

    manager.SetActiveBrightnessPercent(60);

    activity.SetMs(0);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kActive);
    EXPECT_EQ(brightness.last_percent, 60);
}

TEST(PowerManager, IdleDimmingNeverExceedsChosenActiveBrightness) {
    homedeck::EventBus bus;
    RunDispatcherInline(bus);
    FakeUserActivitySource activity;
    FakeDisplayBrightness brightness;
    FakeTimeSource time_source;
    // Below the normal 20% dim level - dimming must not brighten the
    // screen relative to what the user chose for Active.
    homedeck::PowerManager manager(bus, activity, brightness, time_source, 10);

    activity.SetMs(UINT32_MAX);
    bus.Publish(homedeck::ClockTickEvent{});

    EXPECT_EQ(manager.State(), homedeck::PowerState::kIdle);
    EXPECT_EQ(brightness.last_percent, 10);
}
