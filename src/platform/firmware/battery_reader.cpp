#include "platform/firmware/battery_reader.h"

#include "platform/firmware/i2c_device.h"

#include "ina226.hpp"

#include <algorithm>

namespace homedeck {

namespace {

constexpr uint8_t kIna226Address = 0x41;

// NP-F550 is a 2S Li-ion pack (7.4V nominal, see hardware.md#power) -
// these are simple linear-approximation bounds for ReadPercent(), not
// measured/tuned against the real pack's discharge curve.
constexpr float kMinVoltage = 6.4f;  // ~0%: 2 x 3.2V, a conservative cutoff
constexpr float kMaxVoltage = 8.4f;  // 100%: 2 x 4.2V, full charge

}  // namespace

Ina226BatteryReader::Ina226BatteryReader(i2c_master_bus_handle_t i2c_bus)
    : device_(std::make_unique<I2cDevice>(i2c_bus, kIna226Address)) {
    espp::Ina226::Config config{
        .device_address = kIna226Address,
        .write =
            [this](uint8_t addr, const uint8_t* data, size_t len) {
                return device_->Write(addr, data, len);
            },
        .read_register =
            [this](uint8_t addr, uint8_t reg, uint8_t* data, size_t len) {
                return device_->ReadRegister(addr, reg, data, len);
            },
        .write_then_read =
            [this](uint8_t addr, const uint8_t* write_data, size_t write_len, uint8_t* read_data,
                   size_t read_len) {
                return device_->WriteThenRead(addr, write_data, write_len, read_data, read_len);
            },
    };
    sensor_ = std::make_unique<espp::Ina226>(config);
}

Ina226BatteryReader::~Ina226BatteryReader() = default;

int Ina226BatteryReader::ReadPercent() const {
    std::error_code ec;
    float voltage = sensor_->bus_voltage_volts(ec);
    if (ec) {
        // Conservative fallback: report empty rather than a stale or
        // fabricated value when the sensor can't actually be read.
        return 0;
    }
    float fraction = (voltage - kMinVoltage) / (kMaxVoltage - kMinVoltage);
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    return static_cast<int>(fraction * 100.0f + 0.5f);
}

}  // namespace homedeck
