#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/battery_reader.h"

namespace homedeck {

// Published on both edges (unlike LowBatteryMonitor's NotificationEvent,
// which only ever fires on the way down) - PowerManager needs a
// "cleared" signal to know when to leave kError, which a one-shot
// notification can't carry.
struct CriticalBatteryStateChangedEvent {
    bool critical;
};

// Structural twin of LowBatteryMonitor, but for the "about to die, force
// a safe shutdown" fault ADR-0005's Error-state-scope decision names -
// see docs/architecture/power-management.md#status. Also gated on
// external power, unlike LowBatteryMonitor: a critically-low battery
// actively on external power isn't at brownout risk, the same reasoning
// EvaluateOtaGate (core/ota_gate.h) already applies.
class CriticalBatteryMonitor {
public:
    static constexpr int kCriticalThresholdPercent = 5;

    CriticalBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader);

private:
    EventBus& event_bus_;
    BatteryReader& battery_reader_;
    bool already_critical_ = false;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
