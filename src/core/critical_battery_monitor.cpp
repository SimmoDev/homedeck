#include "core/critical_battery_monitor.h"

#include "core/notification.h"

namespace homedeck {

CriticalBatteryMonitor::CriticalBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader)
    : event_bus_(event_bus), battery_reader_(battery_reader) {
    clock_subscription_ = event_bus_.Subscribe<ClockTickEvent>([this](const ClockTickEvent&) {
        if (!battery_reader_.IsBatteryPresent()) {
            already_critical_ = false;
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
