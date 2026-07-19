#pragma once

#include "platform/battery_reader.h"

#include "driver/i2c_master.h"

#include <memory>

namespace espp {
class Ina226;
}

namespace homedeck {

class I2cDevice;

// Reads real battery state via the INA226 power monitor (I2C address
// 0x41 - see docs/architecture/hardware.md's I2C address map) rather
// than the simulator's fixed mock value. INA226 reports voltage/current
// directly, not a percentage - ReadPercent() derives one from bus
// voltage via a simple linear approximation against the NP-F550's 2S
// Li-ion range, not coulomb-counting. hardware.md's Power section
// already flags this as a known simplification - an accurate
// state-of-charge estimate needs more than this, deferred to M2 power
// management work.
//
// Also owns two things unrelated to the INA226 itself, both via the
// same PI4IOE5V6408 IO expander (I2C 0x44) HomeDeck already talks to
// for Wi-Fi power enable - see hardware.md#power: enabling the IP2326
// charge IC (never automatic; the constructor sets this once) and
// reading USB-C connection presence (a direct digital status bit, not
// inferred from current draw).
class Ina226BatteryReader : public BatteryReader {
public:
    explicit Ina226BatteryReader(i2c_master_bus_handle_t i2c_bus);
    ~Ina226BatteryReader() override;

    Ina226BatteryReader(const Ina226BatteryReader&) = delete;
    Ina226BatteryReader& operator=(const Ina226BatteryReader&) = delete;

    int ReadPercent() const override;
    bool IsExternalPowerConnected() const override;
    bool IsBatteryPresent() const override;

private:
    std::unique_ptr<I2cDevice> device_;
    std::unique_ptr<espp::Ina226> sensor_;
};

}  // namespace homedeck
