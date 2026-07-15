#pragma once

#include "core/event_bus.h"
#include "platform/time_source.h"
#include "platform/timer.h"

#include <chrono>

namespace homedeck {

// Deliberately the raw time, not a formatted string - formatting/locale
// is a display concern, not Clock's (see core.md's "Core provides the
// mechanism, [layers above] provide the policy" split, already applied
// to modules).
struct ClockTickEvent {
    std::chrono::system_clock::time_point time;
};

// Time/date services - see core.md's Responsibilities. Publishes a
// ClockTickEvent through the EventBus on a period (default 1s; the
// parameter mainly exists so this class's own test isn't 1+ second per
// run).
class Clock {
public:
    explicit Clock(TimeSource& time_source, EventBus& event_bus,
                    std::chrono::milliseconds tick_period = std::chrono::seconds(1));

private:
    Timer timer_;
};

}  // namespace homedeck
