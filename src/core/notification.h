#pragma once

#include <string>

namespace homedeck {

// See ADR-0005's "Alert-priority wake cycle during Sleeping" decision -
// this urgency distinction is required by Core's notification contract
// from M2 onward, even though the wake cycle itself (Power Management,
// still unbuilt) and the module "check current state" hook (deferred to
// M5's Uptime Kuma module) aren't. kAlertPriority is for monitoring-type
// alerts (starting with Uptime Kuma, per power-management.md) that
// should eventually interrupt Sleeping; everything else is kDeferred,
// surfaced whenever the device is next awake regardless.
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
