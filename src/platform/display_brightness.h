#pragma once

#include <algorithm>

namespace homedeck {

// Display backlight brightness control - see
// docs/architecture/power-management.md for what drives this
// (PowerManager's Active/Idle dimming). Small and virtual, matching
// BatteryReader/AudioOutput's own reasoning.
class DisplayBrightness {
public:
    virtual ~DisplayBrightness() = default;

    // 0-100. Callers are expected to pass an already-clamped value;
    // implementations clamp defensively rather than trust that - via
    // ClampBrightnessPercent() below, not each reimplementing the same
    // std::clamp call.
    virtual void SetPercent(int percent) = 0;
};

// Shared by both HostDisplayBrightness and FirmwareDisplayBrightness -
// factored out here (portable, no LVGL/ESP-IDF dependency) so it's
// host-testable directly, unlike the two implementations themselves.
inline int ClampBrightnessPercent(int percent) { return std::clamp(percent, 0, 100); }

}  // namespace homedeck
