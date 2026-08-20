#include "core/critical_battery_monitor.h"

#include "core/notification.h"

namespace homedeck {

CriticalBatteryMonitor::CriticalBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader)
    : monitor_(
          event_bus, battery_reader,
          [](BatteryReader& reader) {
              return reader.ReadPercent() < kCriticalThresholdPercent && !reader.IsExternalPowerConnected();
          },
          [&event_bus] {
              event_bus.Publish(CriticalBatteryStateChangedEvent{true});
              event_bus.Publish(NotificationEvent{"Battery critically low", NotificationSeverity::kAlertPriority});
          },
          [&event_bus] { event_bus.Publish(CriticalBatteryStateChangedEvent{false}); }) {}

}  // namespace homedeck
