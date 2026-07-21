#include "platform/firmware/network_status.h"

namespace homedeck {

NetworkState FirmwareNetworkStatus::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void FirmwareNetworkStatus::SetConnectionState(bool connected, const std::string& ssid,
                                                const std::string& ip_address) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = {connected, ssid, ip_address};
}

}  // namespace homedeck
