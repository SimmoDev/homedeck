#pragma once

#include <cstdint>

namespace homedeck {

// How long since the user last touched the screen - see
// docs/architecture/power-management.md for what this drives
// (Active/Idle transitions). Small and virtual, matching BatteryReader's
// own reasoning - a simple, rarely-called reader, not a performance-
// sensitive primitive, and mockable matters directly for testing
// PowerManager's transition logic without a real display.
class UserActivitySource {
public:
    virtual ~UserActivitySource() = default;

    // Milliseconds since the last touch/input event, or since this
    // object's underlying display was created if none has occurred yet.
    virtual uint32_t MillisecondsSinceLastActivity() const = 0;
};

}  // namespace homedeck
