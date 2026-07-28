#pragma once

namespace homedeck {

// Display backlight brightness control - see
// docs/architecture/power-management.md for what drives this
// (PowerManager's Active/Idle dimming). Small and virtual, matching
// BatteryReader/AudioOutput's own reasoning.
class DisplayBrightness {
public:
    virtual ~DisplayBrightness() = default;

    // 0-100. Callers are expected to pass an already-clamped value;
    // implementations clamp defensively rather than trust that.
    virtual void SetPercent(int percent) = 0;
};

}  // namespace homedeck
