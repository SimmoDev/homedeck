#include "platform/firmware/time_source.h"

#include "platform/firmware/i2c_device.h"

#include "rx8130ce.hpp"

#include <ctime>

namespace homedeck {

namespace {
constexpr uint8_t kRx8130Address = 0x32;
}  // namespace

Rx8130TimeSource::Rx8130TimeSource(i2c_master_bus_handle_t i2c_bus)
    : device_(std::make_unique<I2cDevice>(i2c_bus, kRx8130Address)) {
    espp::Rx8130ce<true>::Config config{
        .device_address = kRx8130Address,
        .write =
            [this](uint8_t addr, const uint8_t* data, size_t len) {
                return device_->Write(addr, data, len);
            },
        .read =
            [this](uint8_t addr, uint8_t* data, size_t len) {
                return device_->Read(addr, data, len);
            },
    };
    rtc_ = std::make_unique<espp::Rx8130ce<true>>(config);
}

Rx8130TimeSource::~Rx8130TimeSource() = default;

std::chrono::system_clock::time_point Rx8130TimeSource::Now() const {
    std::error_code ec;
    std::tm tm = rtc_->get_time(ec);
    if (ec) {
        // Fallback if the RTC can't be read: whatever the system clock
        // currently holds (likely wrong/near-epoch on firmware, since
        // nothing sets it from another source yet) - imperfect, but
        // better than crashing or returning a fixed sentinel a caller
        // might mistake for a real time.
        return std::chrono::system_clock::now();
    }
    std::time_t t = std::mktime(&tm);
    return std::chrono::system_clock::from_time_t(t);
}

bool Rx8130TimeSource::SetTime(std::time_t utc_time) {
    std::tm tm{};
    gmtime_r(&utc_time, &tm);
    std::error_code ec;
    rtc_->set_time(tm, ec);
    return !ec;
}

}  // namespace homedeck
