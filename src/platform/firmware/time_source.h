#pragma once

#include "platform/time_source.h"

#include "driver/i2c_master.h"

#include <ctime>
#include <memory>
#include <mutex>

namespace espp {
template <bool UseAddress>
class Rx8130ce;
}

namespace homedeck {

class I2cDevice;

// Reads real wall-clock time via the RX8130CE RTC (I2C address 0x32 -
// see docs/architecture/hardware.md's I2C address map) rather than the
// simulator's std::chrono::system_clock::now(). Interprets the RTC's
// reported time as local wall time directly (via std::mktime) - no
// timezone handling exists yet anywhere in this project; that's a known
// gap, not something this class tries to solve on its own.
//
// Thread-safe: Now() and SetTime() both lock mutex_ around the actual
// I2C access - genuinely needed, not defensive, since ADR-0028's SNTP
// sync callback calls SetTime() from LwIP's own task while Clock's
// Timer keeps calling Now() from the FreeRTOS timer service task,
// unsynchronized without this. espp::Rx8130ce reads/writes several
// registers per call (year/month/day/hour/min/sec are separate BCD
// fields on this chip) - an unguarded interleaving could hand a caller
// a torn read (some fields from before a concurrent write, some from
// after), not just a stale one.
class Rx8130TimeSource : public TimeSource {
public:
    explicit Rx8130TimeSource(i2c_master_bus_handle_t i2c_bus);
    ~Rx8130TimeSource() override;

    Rx8130TimeSource(const Rx8130TimeSource&) = delete;
    Rx8130TimeSource& operator=(const Rx8130TimeSource&) = delete;

    std::chrono::system_clock::time_point Now() const override;

    // Writes utc_time into the physical RTC via I2C, so the correction
    // survives a reboot on the supercap backup rather than needing to be
    // reapplied every boot - see ADR-0028 for why SNTP (the only current
    // caller, firmware/main/homedeck.cpp) is what calls this. Broken down
    // via gmtime_r, not localtime_r, deliberately matching Now()'s own
    // "no timezone handling" convention above: Now() feeds the RTC's raw
    // fields into mktime() with no TZ offset applied, so writing back the
    // same raw UTC fields here (rather than applying a TZ conversion this
    // project doesn't have) is what actually round-trips correctly given
    // that existing behavior, not a second, inconsistent interpretation
    // of what the RTC's fields mean. Returns false if the I2C write
    // itself fails; the RTC is left holding whatever it held before.
    bool SetTime(std::time_t utc_time);

private:
    mutable std::mutex mutex_;
    std::unique_ptr<I2cDevice> device_;
    std::unique_ptr<espp::Rx8130ce<true>> rtc_;
};

}  // namespace homedeck
