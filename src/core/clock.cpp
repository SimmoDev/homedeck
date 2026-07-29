#include "core/clock.h"

namespace homedeck {

Clock::Clock(TimeSource& time_source, EventBus& event_bus, std::chrono::milliseconds tick_period)
    : time_source_(time_source),
      event_bus_(event_bus),
      timer_("clock", tick_period, [&time_source, &event_bus]() {
          event_bus.Publish(ClockTickEvent{time_source.Now()});
      }) {}

void Clock::Start() { event_bus_.Publish(ClockTickEvent{time_source_.Now()}); }

}  // namespace homedeck
