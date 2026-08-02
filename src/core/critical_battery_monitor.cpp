#include "core/critical_battery_monitor.h"

#include "core/notification.h"

namespace homedeck {

CriticalBatteryMonitor::CriticalBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader)
    : event_bus_(event_bus), battery_reader_(battery_reader) {
    clock_subscription_ = event_bus_.Subscribe<ClockTickEvent>([this](const ClockTickEvent&) {
        if (!battery_reader_.IsBatteryPresent()) {
            // Publish the clearing edge too, not just reset the latch - if
            // the battery was critical (PowerManager already in kError),
            // the only way it leaves kError is this event. Resetting
            // already_critical_ without publishing would strand
            // PowerManager in kError permanently: the next tick with a
            // freshly-inserted healthy battery would see critical==false
            // but already_critical_ already false too, so neither branch
            // below would fire and the clearing event would never be sent.
            if (already_critical_) {
                already_critical_ = false;
                event_bus_.Publish(CriticalBatteryStateChangedEvent{false});
            }
            return;
        }
        bool critical = battery_reader_.ReadPercent() < kCriticalThresholdPercent &&
                         !battery_reader_.IsExternalPowerConnected();
        if (critical && !already_critical_) {
            already_critical_ = true;
            event_bus_.Publish(CriticalBatteryStateChangedEvent{true});
            event_bus_.Publish(NotificationEvent{"Battery critically low", NotificationSeverity::kAlertPriority});
        } else if (!critical && already_critical_) {
            already_critical_ = false;
            event_bus_.Publish(CriticalBatteryStateChangedEvent{false});
        }
    });
}

}  // namespace homedeck
