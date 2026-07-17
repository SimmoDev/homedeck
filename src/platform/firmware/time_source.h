#pragma once

#include "platform/time_source.h"

#include "driver/i2c_master.h"

#include <memory>

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
class Rx8130TimeSource : public TimeSource {
public:
    explicit Rx8130TimeSource(i2c_master_bus_handle_t i2c_bus);
    ~Rx8130TimeSource() override;

    Rx8130TimeSource(const Rx8130TimeSource&) = delete;
    Rx8130TimeSource& operator=(const Rx8130TimeSource&) = delete;

    std::chrono::system_clock::time_point Now() const override;

private:
    std::unique_ptr<I2cDevice> device_;
    std::unique_ptr<espp::Rx8130ce<true>> rtc_;
};

}  // namespace homedeck
