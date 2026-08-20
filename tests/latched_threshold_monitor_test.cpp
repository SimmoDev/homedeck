#include "core/latched_threshold_monitor.h"

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

// Directly exercises the shared state machine LowBatteryMonitor/
// CriticalBatteryMonitor are both built on (see the class's own header
// comment) - a regression here would otherwise only surface as a
// failure in one of those two unrelated-looking test suites, with
// nothing pointing at the actual broken component.
struct Monitor {
    homedeck::EventBus bus;
    FakeBatteryReader battery;
    int entering_count = 0;
    int leaving_count = 0;
    bool is_bad = false;
    homedeck::LatchedThresholdMonitor monitor{
        bus, battery, [this](homedeck::BatteryReader&) { return is_bad; }, [this] { entering_count++; },
        [this] { leaving_count++; }};

    void Tick() { bus.Publish(homedeck::ClockTickEvent{}); }
};

}  // namespace

TEST(LatchedThresholdMonitor, DoesNotFireWhileNeverBad) {
    Monitor m;
    m.Tick();
    m.Tick();
    EXPECT_EQ(m.entering_count, 0);
    EXPECT_EQ(m.leaving_count, 0);
}

TEST(LatchedThresholdMonitor, FiresOnEnteringAndLatchesAgainstRepeatTicks) {
    Monitor m;
    m.is_bad = true;
    m.Tick();
    m.Tick();
    m.Tick();
    EXPECT_EQ(m.entering_count, 1);
    EXPECT_EQ(m.leaving_count, 0);
}

TEST(LatchedThresholdMonitor, FiresOnLeavingAfterEntering) {
    Monitor m;
    m.is_bad = true;
    m.Tick();
    m.is_bad = false;
    m.Tick();
    EXPECT_EQ(m.entering_count, 1);
    EXPECT_EQ(m.leaving_count, 1);
}

TEST(LatchedThresholdMonitor, RefiresOnEnteringAfterARecoveryCycle) {
    Monitor m;
    m.is_bad = true;
    m.Tick();
    m.is_bad = false;
    m.Tick();
    m.is_bad = true;
    m.Tick();
    EXPECT_EQ(m.entering_count, 2);
    EXPECT_EQ(m.leaving_count, 1);
}

TEST(LatchedThresholdMonitor, BatteryAbsentWhileNotBadDoesNothing) {
    Monitor m;
    m.battery.SetBatteryPresent(false);
    m.Tick();
    EXPECT_EQ(m.entering_count, 0);
    EXPECT_EQ(m.leaving_count, 0);
}

TEST(LatchedThresholdMonitor, BatteryRemovedWhileBadFiresLeavingWithoutEvaluatingIsBad) {
    Monitor m;
    m.is_bad = true;
    m.Tick();  // enters bad
    m.battery.SetBatteryPresent(false);
    m.Tick();
    EXPECT_EQ(m.entering_count, 1);
    EXPECT_EQ(m.leaving_count, 1);
}

TEST(LatchedThresholdMonitor, BatteryReinsertedStillBadFiresEnteringAgainRatherThanStayingLatchedShut) {
    Monitor m;
    m.is_bad = true;
    m.Tick();  // enters bad
    m.battery.SetBatteryPresent(false);
    m.Tick();  // leaves (latch reset), since presence can't evaluate is_bad
    m.battery.SetBatteryPresent(true);
    // Still bad once re-evaluated with the battery back - a fresh
    // entering edge, not silently staying latched from before removal.
    m.Tick();
    EXPECT_EQ(m.entering_count, 2);
    EXPECT_EQ(m.leaving_count, 1);
}
