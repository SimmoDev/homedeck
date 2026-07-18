#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/battery_reader.h"

namespace homedeck {

// Publishes a low-battery NotificationEvent once when the battery
// crosses below kThresholdPercent, not on every tick while it stays low
// - latched so a sustained low-battery state doesn't spam a
// notification once a second. The latch resets once the battery goes
// back above threshold (e.g. after recharging), so a second real
// low-battery episode notifies again.
class LowBatteryMonitor {
public:
    static constexpr int kThresholdPercent = 15;

    LowBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader);

private:
    EventBus& event_bus_;
    BatteryReader& battery_reader_;
    bool already_notified_ = false;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
