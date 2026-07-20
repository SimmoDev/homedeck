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

// IsBatteryPresent() uses current as its primary signal, not voltage -
// confirmed on hardware that bus_voltage_volts() alone cannot
// distinguish "no battery" from "battery present": with the IP2326
// charge IC enabled (see the constructor below) and nothing connected
// to charge, the charger hunts for its regulation target on the
// unloaded output, swinging between roughly 4V and kMaxVoltage (8.4V)
// every tick rather than settling. current_amps() doesn't have this
// problem in general: with no battery it reads a flat 0.000000A (no
// load to source/sink current), and settles to a small but clearly
// nonzero, stable value (~0.02A on the reference unit) within one tick
// of a real pack being connected - see hardware.md#power.
constexpr float kBatteryPresentCurrentThresholdAmps = 0.005f;

// Current alone still isn't sufficient once a battery reaches full
// charge: the IP2326 stops driving any current once charging
// terminates, so a fully-charged idle battery also reads a literal
// 0.000000A - confirmed on hardware, indistinguishable from "no
// battery" by current alone. Voltage is the only remaining signal in
// that state, and unlike the no-battery case above, it's usable there:
// an installed battery clamps the rail via its own chemistry, so
// consecutive readings stay within tens of mV of each other even right
// at the charging-terminates transition (confirmed on hardware: ~0.02V
// between samples), nowhere near the multi-volt hunting swing a truly
// empty charge path produces every tick. This is a >60x margin between
// the two cases, wide enough for a single-sample delta check.
constexpr float kVoltageStabilityThresholdVolts = 0.5f;

// Both bits are on the PI4IOE5V6408 IO expander at I2C 0x44 - the same
// chip and instance (bsp_io_expander1_init()) HomeDeck already uses for
// Wi-Fi power enable. Confirmed against M5Stack's official Tab5 pinmap
// (C145_Pinmap_Overview.png, docs.m5stack.com): E2.P7 is CHG_EN, E2.P6
// is CHG_STAT - see hardware.md#power.
constexpr uint32_t kChargeEnablePin = IO_EXPANDER_PIN_NUM_7;   // output
// This is the IP2326's charge-status output, not a raw cable-presence
// pin despite M5Tab5-UserDemo's own bsp_usb_c_detect() naming (its
// m5stack_tab5.c reads this same bit) - it's high only while the
// charger is actively driving current into a battery, and goes low
// once charging terminates (e.g. a full battery), even with the cable
// still connected. There is no separate raw VBUS-presence pin exposed
// via this IO expander on the official pinmap. IsExternalPowerConnected()
// combines this with IsBatteryPresent() to also cover the no-battery,
// USB-C-only case, where this bit alone would never assert.
constexpr uint32_t kChargeStatusPin = IO_EXPANDER_PIN_NUM_6;  // input

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

    // Set once here, not on every IsExternalPowerConnected() call, matching
    // how kChargeEnablePin above is also only ever configured once.
    esp_err_t status_err = esp_io_expander_set_dir(io_expander, kChargeStatusPin, IO_EXPANDER_INPUT);
    // A pull-down, not left floating - confirmed against M5Stack's own
    // M5Tab5-UserDemo reference firmware (bsp_io_expander_pi4ioe_init() in
    // its m5stack_tab5.c), which explicitly enables one on this exact pin.
    // Without it, an idle input floats and can read a brief spurious high
    // from ESD/EMI right as a USB-C connector mates, then decay back down
    // - observed on hardware before this was added.
    status_err |= esp_io_expander_set_pullupdown(io_expander, kChargeStatusPin, IO_EXPANDER_PULL_DOWN);
    if (status_err != ESP_OK) {
        printf("Ina226BatteryReader: failed to configure charge status pin\n");
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
    if (std::fabs(current) >= kBatteryPresentCurrentThresholdAmps) {
        return true;
    }

    // Ambiguous zone (see kVoltageStabilityThresholdVolts's comment) -
    // fall back to voltage stability.
    float voltage = sensor_->bus_voltage_volts(ec);
    if (ec) {
        return false;
    }
    bool stable = has_last_voltage_ && std::fabs(voltage - last_voltage_volts_) < kVoltageStabilityThresholdVolts;
    last_voltage_volts_ = voltage;
    has_last_voltage_ = true;
    return stable;
}

bool Ina226BatteryReader::IsExternalPowerConnected() const {
    esp_io_expander_handle_t io_expander = bsp_io_expander1_init();
    bool charging = false;
    if (io_expander != nullptr) {
        uint32_t level_mask = 0;
        if (esp_io_expander_get_level(io_expander, kChargeStatusPin, &level_mask) == ESP_OK) {
            charging = (level_mask & kChargeStatusPin) != 0;
        }
    }
    // A running device with no battery has no other possible power
    // source - see kChargeStatusPin's comment for why that case can't
    // be read directly off the charge-status signal itself.
    return charging || !IsBatteryPresent();
}

}  // namespace homedeck
