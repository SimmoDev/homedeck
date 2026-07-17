#pragma once

#include "driver/i2c_master.h"

#include <cstddef>
#include <cstdint>

namespace homedeck {

// Minimal I2C glue matching the espp peripheral library's write/read/
// probe function-pointer shape (see BasePeripheral in the espp/ina226
// and espp/rx8130ce components this project depends on) - shared rather
// than duplicated, since BatteryReader and TimeSource's firmware
// backends both need the exact same handful of ESP-IDF i2c_master
// calls, just against different fixed addresses on the same shared bus
// (see bsp_i2c_get_handle()).
class I2cDevice {
public:
    I2cDevice(i2c_master_bus_handle_t bus, uint8_t address);
    ~I2cDevice();

    I2cDevice(const I2cDevice&) = delete;
    I2cDevice& operator=(const I2cDevice&) = delete;

    // Each method's leading device_address parameter is unused - the
    // espp peripheral libraries pass it back from their own Config
    // regardless, but this instance is already bound to one fixed
    // address at construction, so there's nothing to look up.
    bool Write(uint8_t device_address, const uint8_t* data, size_t len);
    bool Read(uint8_t device_address, uint8_t* data, size_t len);
    bool ReadRegister(uint8_t device_address, uint8_t reg, uint8_t* data, size_t len);
    bool WriteThenRead(uint8_t device_address, const uint8_t* write_data, size_t write_len,
                        uint8_t* read_data, size_t read_len);

private:
    i2c_master_dev_handle_t handle_;
};

}  // namespace homedeck
