#include "core/clock.h"

namespace homedeck {

Clock::Clock(TimeSource& time_source, EventBus& event_bus, std::chrono::milliseconds tick_period)
    : timer_("clock", tick_period, [&time_source, &event_bus]() {
          event_bus.Publish(ClockTickEvent{time_source.Now()});
      }) {
    // Timer only fires after a full period elapses (see platform/timer.h),
    // which would otherwise leave any subscriber - e.g. a freshly created
    // clock label - showing nothing meaningful for up to tick_period.
    // Publish once immediately so current time is available right away.
    event_bus.Publish(ClockTickEvent{time_source.Now()});
}

}  // namespace homedeck
