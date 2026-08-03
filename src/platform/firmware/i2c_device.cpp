#include "platform/firmware/i2c_device.h"

#include "esp_err.h"

namespace homedeck {

namespace {
constexpr int kTimeoutMs = 1000;
constexpr uint32_t kSclSpeedHz = 400000;
}  // namespace

// i2c_master_bus_add_device() only registers this address in the driver's
// own software device table - it doesn't talk to the bus at all, so its
// failure modes are a duplicate address already registered (a
// programming bug: this project's device addresses are fixed constants,
// never runtime-derived) or the driver being out of heap - not "the
// physical device is absent," which is instead something BatteryReader/
// TimeSource's own Read()/Write() call sites already degrade gracefully
// against (see their "conservative fallback" comments) once real bus
// transactions are actually attempted. ESP_ERROR_CHECK here is
// deliberate, not a gap in that graceful-degradation story.
I2cDevice::I2cDevice(i2c_master_bus_handle_t bus, uint8_t address) {
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = kSclSpeedHz,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &handle_));
}

I2cDevice::~I2cDevice() { i2c_master_bus_rm_device(handle_); }

bool I2cDevice::Write(uint8_t /*device_address*/, const uint8_t* data, size_t len) {
    return i2c_master_transmit(handle_, data, static_cast<size_t>(len), kTimeoutMs) == ESP_OK;
}

bool I2cDevice::Read(uint8_t /*device_address*/, uint8_t* data, size_t len) {
    return i2c_master_receive(handle_, data, len, kTimeoutMs) == ESP_OK;
}

bool I2cDevice::ReadRegister(uint8_t /*device_address*/, uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_transmit_receive(handle_, &reg, 1, data, len, kTimeoutMs) == ESP_OK;
}

bool I2cDevice::WriteThenRead(uint8_t /*device_address*/, const uint8_t* write_data,
                               size_t write_len, uint8_t* read_data, size_t read_len) {
    return i2c_master_transmit_receive(handle_, write_data, write_len, read_data, read_len,
                                        kTimeoutMs) == ESP_OK;
}

}  // namespace homedeck
