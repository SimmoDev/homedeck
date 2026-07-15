#pragma once

#include <chrono>

namespace homedeck {

// Current time access - see docs/architecture/overview.md#hardware-abstraction.
// Small and virtual for the same reason as BatteryReader: a simple,
// rarely-called reader, not a performance-sensitive primitive, and
// mockable matters directly (a fixed-time fake makes Clock's own test
// deterministic instead of depending on real wall-clock time).
class TimeSource {
public:
    virtual ~TimeSource() = default;
    virtual std::chrono::system_clock::time_point Now() const = 0;
};

}  // namespace homedeck
