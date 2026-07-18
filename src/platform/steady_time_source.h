#pragma once

#include "platform/time_source.h"

namespace homedeck {

// A monotonic clock, reinterpreted as TimeSource's system_clock::time_point
// shape purely for duration arithmetic - never displayed as a wall-clock
// date. For callers that need reliable elapsed-time comparisons (e.g.
// AdminAuthService's session expiry) rather than an actual calendar date,
// where the wall-clock TimeSource implementations aren't trustworthy: on
// firmware, Rx8130TimeSource reads the RTC over I2C on every call, and
// that RTC has a known, pre-existing gap (never calibrated - see
// hardware.md) with no guarantee its readings are even self-consistent
// call to call, let alone correct, until that's fixed. std::chrono::
// steady_clock has neither problem - it's standard C++, identical on
// host and firmware (ESP-IDF's newlib backs it with esp_timer), so no
// host/firmware split is needed here the way most platform/ interfaces
// require.
class SteadyTimeSource : public TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override {
        return std::chrono::system_clock::time_point(std::chrono::steady_clock::now().time_since_epoch());
    }
};

}  // namespace homedeck
