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
};

}  // namespace homedeck
