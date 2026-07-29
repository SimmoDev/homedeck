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

    // Publishes the first tick immediately, rather than leaving
    // subscribers - e.g. a freshly created clock label - showing
    // nothing meaningful for up to tick_period. Deliberately not done
    // in the constructor: that would make every ClockTickEvent
    // subscriber's construction-order relative to Clock's own load-
    // bearing (a subscriber constructed after Clock silently misses this
    // tick, with nothing to catch the mistake). Call once, after every
    // subscriber that needs this first tick already exists - construction
    // order no longer matters as long as this call comes last.
    void Start();

private:
    TimeSource& time_source_;
    EventBus& event_bus_;
    Timer timer_;
};

}  // namespace homedeck
