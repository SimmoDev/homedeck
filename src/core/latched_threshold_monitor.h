#pragma once

#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/battery_reader.h"

#include <functional>

namespace homedeck {

// The shared "evaluate a battery condition once a tick, latch so a
// sustained bad state doesn't republish every tick" state machine
// CriticalBatteryMonitor and LowBatteryMonitor are both built on - see
// each one's own comment for how their conditions/publishing genuinely
// differ (external-power gating, one- vs. two-edge publishing). Not a
// general pub/sub abstraction - deliberately narrow to this one repeated
// shape, not meant to grow new callers casually.
//
// While no battery is present, "bad" can't be evaluated at all - the
// latch just tracks whichever edge already-bad state implies: leaving
// (on_leaving fires) if it was bad, nothing if it wasn't (so a battery
// reinserted already-bad still notifies, rather than starting latched
// shut from before it was removed).
class LatchedThresholdMonitor {
public:
    LatchedThresholdMonitor(EventBus& event_bus, BatteryReader& battery_reader, std::function<bool(BatteryReader&)> is_bad,
                             std::function<void()> on_entering, std::function<void()> on_leaving);

private:
    bool already_bad_ = false;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
