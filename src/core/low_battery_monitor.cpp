#include "core/low_battery_monitor.h"

#include "core/notification.h"

namespace homedeck {

LowBatteryMonitor::LowBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader)
    : event_bus_(event_bus), battery_reader_(battery_reader) {
    clock_subscription_ = event_bus_.Subscribe<ClockTickEvent>([this](const ClockTickEvent&) {
        if (!battery_reader_.IsBatteryPresent()) {
            // Not low - there's nothing to be low. Reset the latch too,
            // so a battery inserted later while genuinely low still
            // notifies, rather than staying suppressed from before it
            // was removed.
            already_notified_ = false;
            return;
        }
        int percent = battery_reader_.ReadPercent();
        if (percent < kThresholdPercent) {
            if (!already_notified_) {
                already_notified_ = true;
                event_bus_.Publish(NotificationEvent{"Battery low", NotificationSeverity::kDeferred});
            }
        } else {
            already_notified_ = false;
        }
    });
}

}  // namespace homedeck
