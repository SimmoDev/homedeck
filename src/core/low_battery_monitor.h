#pragma once

#include "core/event_bus.h"
#include "core/latched_threshold_monitor.h"
#include "platform/battery_reader.h"

namespace homedeck {

// Publishes a low-battery NotificationEvent once when the battery
// crosses below kThresholdPercent, not on every tick while it stays low
// - latched (via LatchedThresholdMonitor) so a sustained low-battery
// state doesn't spam a notification once a second. The latch resets
// once the battery goes back above threshold (e.g. after recharging),
// so a second real low-battery episode notifies again. One-directional
// only - there's no "recovered" notification to publish on the
// recovery edge, unlike CriticalBatteryMonitor's own
// CriticalBatteryStateChangedEvent, which PowerManager needs both edges
// of.
class LowBatteryMonitor {
public:
    static constexpr int kThresholdPercent = 15;

    LowBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader);

private:
    LatchedThresholdMonitor monitor_;
};

}  // namespace homedeck
