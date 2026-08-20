#include "core/low_battery_monitor.h"

#include "core/notification.h"

namespace homedeck {

LowBatteryMonitor::LowBatteryMonitor(EventBus& event_bus, BatteryReader& battery_reader)
    : monitor_(
          event_bus, battery_reader,
          [](BatteryReader& reader) { return reader.ReadPercent() < kThresholdPercent; },
          [&event_bus] { event_bus.Publish(NotificationEvent{"Battery low", NotificationSeverity::kDeferred}); },
          [] {}) {}

}  // namespace homedeck
