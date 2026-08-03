#pragma once

#include "platform/battery_reader.h"

#include <string>

namespace homedeck {

// A firmware update in progress is not power-safe to interrupt - a lost
// OTA write can leave the device unbootable. Per
// docs/decisions/ADR-0005-power-and-sleep-model.md's OTA gate decision,
// updates are only allowed with enough battery margin to finish
// uninterrupted, or when external power removes that risk entirely.
struct OtaGateStatus {
    bool open = false;
    // Empty when open; a user-facing explanation when closed, per
    // ADR-0005's "deferred with a clear explanation, not silently
    // blocked."
    std::string reason;
    // The battery reading this decision was actually based on - callers
    // that also report battery percent (e.g. GET /api/ota/status) should
    // use this rather than taking a second, independent ReadPercent()
    // call, which could disagree with `open` at a threshold boundary.
    int battery_percent = 0;
};

inline OtaGateStatus EvaluateOtaGate(const BatteryReader& battery_reader) {
    constexpr int kMinBatteryPercent = 30;
    int battery_percent = battery_reader.ReadPercent();

    if (battery_percent >= kMinBatteryPercent) {
        return {true, "", battery_percent};
    }
    if (battery_reader.IsExternalPowerConnected()) {
        return {true, "", battery_percent};
    }
    return {false,
            "Battery is below " + std::to_string(kMinBatteryPercent) +
                "% and no external power is connected. Connect a charger to update.",
            battery_percent};
}

}  // namespace homedeck
