#include "core/latched_threshold_monitor.h"

namespace homedeck {

LatchedThresholdMonitor::LatchedThresholdMonitor(EventBus& event_bus, BatteryReader& battery_reader,
                                                   std::function<bool(BatteryReader&)> is_bad, std::function<void()> on_entering,
                                                   std::function<void()> on_leaving) {
    clock_subscription_ = event_bus.Subscribe<ClockTickEvent>(
        [this, &battery_reader, is_bad = std::move(is_bad), on_entering = std::move(on_entering),
         on_leaving = std::move(on_leaving)](const ClockTickEvent&) {
            if (!battery_reader.IsBatteryPresent()) {
                if (already_bad_) {
                    already_bad_ = false;
                    on_leaving();
                }
                return;
            }
            bool bad = is_bad(battery_reader);
            if (bad && !already_bad_) {
                already_bad_ = true;
                on_entering();
            } else if (!bad && already_bad_) {
                already_bad_ = false;
                on_leaving();
            }
        });
}

}  // namespace homedeck
