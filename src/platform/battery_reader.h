#pragma once

namespace homedeck {

// Battery level access - see docs/architecture/overview.md#hardware-abstraction.
// Small and virtual (unlike Task/Timer's pImpl'd backends) because it's a
// simple, rarely-called data reader, not a performance-sensitive
// primitive, and being mockable matters directly (e.g. testing low-
// battery presentation later without needing a real battery state).
class BatteryReader {
public:
    virtual ~BatteryReader() = default;

    // 0-100.
    virtual int ReadPercent() const = 0;

    // True when external USB-C power is present, independent of battery
    // percentage.
    virtual bool IsExternalPowerConnected() const = 0;

    // True when a battery pack is physically installed. HomeDeck must
    // run on a battery-less USB-C-only unit (see
    // docs/architecture/hardware.md#battery-optional-operation), so
    // ReadPercent() alone isn't enough - a missing battery must read as
    // "no battery," never as "battery critically low."
    virtual bool IsBatteryPresent() const = 0;
};

}  // namespace homedeck
