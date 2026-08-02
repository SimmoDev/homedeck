#pragma once

#include <string>

namespace homedeck {

// This urgency distinction predates ADR-0024, which removed the
// wake-cycle design ADR-0005 originally motivated it with - Sleeping
// has no RTC wake cycle to interrupt, so the distinction carries no
// wake-cycle behavior today. It remains available for a future
// presentation difference (see ui.md#notification-presentation) and
// already has a real publisher: CriticalBatteryMonitor publishes
// kAlertPriority (see power-management.md#status); everything else is
// kDeferred, surfaced whenever the device is next awake regardless.
enum class NotificationSeverity {
    kDeferred,
    kAlertPriority,
};

// Deliberately just a message and a severity - no source/category field,
// since Core's notification service has no concept of which module (or
// Core facility) a notification came from; per core.md's module
// boundary, the publisher already reduces its own domain knowledge
// ("Uptime Kuma monitor down") to this generic shape before publishing.
struct NotificationEvent {
    std::string message;
    NotificationSeverity severity;
};

}  // namespace homedeck
