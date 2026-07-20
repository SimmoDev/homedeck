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
};

inline OtaGateStatus EvaluateOtaGate(const BatteryReader& battery_reader) {
    constexpr int kMinBatteryPercent = 30;

    if (battery_reader.ReadPercent() >= kMinBatteryPercent) {
        return {true, ""};
    }
    if (battery_reader.IsExternalPowerConnected()) {
        return {true, ""};
    }
    return {false, "Battery is below " + std::to_string(kMinBatteryPercent) +
                        "% and no external power is connected. Connect a charger to update."};
}

}  // namespace homedeck
