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

    // True when external power is confirmed present - actively charging a
    // present battery, or no battery installed (the only cases this can be
    // determined from available signals). False does NOT guarantee external
    // power is absent: a battery that's connected but not currently drawing
    // charge current (e.g. topped off) is indistinguishable from being
    // unplugged on this hardware - see
    // Ina226BatteryReader::IsExternalPowerConnected() for the full reasoning.
    virtual bool IsExternalPowerConnected() const = 0;

    // True when a battery pack is physically installed. HomeDeck must
    // run on a battery-less USB-C-only unit (see
    // docs/architecture/hardware.md#battery-optional-operation), so
    // ReadPercent() alone isn't enough - a missing battery must read as
    // "no battery," never as "battery critically low."
    virtual bool IsBatteryPresent() const = 0;
};

}  // namespace homedeck
