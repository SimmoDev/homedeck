#include "platform/firmware/battery_reader.h"

#include "platform/firmware/i2c_device.h"

#include "bsp/m5stack_tab5.h"
#include "esp_io_expander.h"
#include "ina226.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace homedeck {

namespace {

constexpr uint8_t kIna226Address = 0x41;

// NP-F550 is a 2S Li-ion pack (7.4V nominal, see hardware.md#power) -
// these are simple linear-approximation bounds for ReadPercent(), not
// measured/tuned against the real pack's discharge curve.
constexpr float kMinVoltage = 6.4f;  // ~0%: 2 x 3.2V, a conservative cutoff
constexpr float kMaxVoltage = 8.4f;  // 100%: 2 x 4.2V, full charge

// IsBatteryPresent() uses current, not voltage - confirmed on hardware
// that bus_voltage_volts() cannot distinguish "no battery" from
// "battery present": with the IP2326 charge IC enabled (see the
// constructor below) and nothing connected to charge, the charger
// hunts for its regulation target on the unloaded output, swinging
// between roughly 4V and kMaxVoltage (8.4V) every tick rather than
// settling - readings in that range are as likely to come from an
// empty charge path as from a real battery. current_amps() doesn't
// have this problem: with no battery it reads a flat 0.000000A (no
// load to source/sink current), and settles to a small but clearly
// nonzero, stable value (~0.02A on the reference unit) within one tick
// of a real pack being connected. Known limitation, matching
// IsExternalPowerConnected()'s own: a battery at 100% may taper its
// maintenance current below this threshold, under-reporting "present" -
// see hardware.md#power.
constexpr float kBatteryPresentCurrentThresholdAmps = 0.005f;

// Both bits are on the PI4IOE5V6408 IO expander at I2C 0x44 - the same
// chip and instance (bsp_io_expander1_init()) HomeDeck already uses for
// Wi-Fi power enable. Confirmed against M5Stack's own M5Tab5-UserDemo
// reference firmware (bsp_set_charge_en()/bsp_usb_c_detect() in its
// m5stack_tab5.c), which this project's vendored espressif/m5stack_tab5
// BSP component doesn't itself expose - see hardware.md#power.
constexpr uint32_t kChargeEnablePin = IO_EXPANDER_PIN_NUM_7;   // output
constexpr uint32_t kUsbCDetectPin = IO_EXPANDER_PIN_NUM_6;     // input

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

    // The IP2326 charge IC's enable line is gated by this bit, not
    // automatic - without this, the battery does not charge over USB-C
    // even while connected.
    esp_io_expander_handle_t io_expander = bsp_io_expander1_init();
    if (io_expander == nullptr) {
        printf("Ina226BatteryReader: bsp_io_expander1_init() failed - charging not enabled\n");
        return;
    }
    esp_err_t err = esp_io_expander_set_dir(io_expander, kChargeEnablePin, IO_EXPANDER_OUTPUT);
    // Pins reset to open-drain/high-impedance (not actively driven) -
    // set_level() alone leaves the pin floating without this, matching
    // the exact pattern bsp_feature_en.c already uses for every other
    // enable pin (LCD/TOUCH/SPEAKER/CAMERA/USB/WIFI).
    err |= esp_io_expander_set_output_mode(io_expander, kChargeEnablePin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    err |= esp_io_expander_set_level(io_expander, kChargeEnablePin, 1);
    if (err != ESP_OK) {
        printf("Ina226BatteryReader: failed to enable charging (err=%d)\n", err);
    }
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

bool Ina226BatteryReader::IsBatteryPresent() const {
    std::error_code ec;
    float current = sensor_->current_amps(ec);
    if (ec) {
        // Conservative fallback, matching ReadPercent(): assume no
        // battery rather than fabricate a reading the sensor couldn't
        // actually take.
        return false;
    }
    return std::fabs(current) >= kBatteryPresentCurrentThresholdAmps;
}

bool Ina226BatteryReader::IsExternalPowerConnected() const {
    esp_io_expander_handle_t io_expander = bsp_io_expander1_init();
    if (io_expander == nullptr) {
        return false;
    }
    if (esp_io_expander_set_dir(io_expander, kUsbCDetectPin, IO_EXPANDER_INPUT) != ESP_OK) {
        return false;
    }
    uint32_t level_mask = 0;
    if (esp_io_expander_get_level(io_expander, kUsbCDetectPin, &level_mask) != ESP_OK) {
        return false;
    }
    return (level_mask & kUsbCDetectPin) != 0;
}

}  // namespace homedeck
