#pragma once

#include "platform/network_status.h"

namespace homedeck {

// A mock value, adjustable via the setters below - defaults to
// connected with plausible-looking SSID/IP, since most UI development
// wants a normal "online" dashboard, not a disconnected one. No
// simulator debug-toggle UI exists yet (unlike HostBatteryReader's
// low-battery test trigger) - nothing currently needs interactive
// Wi-Fi-state testing; add one the same way if a real need shows up.
class HostNetworkStatus : public NetworkStatus {
public:
    NetworkState Snapshot() const override { return state_; }

    void SetConnected(bool connected) { state_.connected = connected; }
    void SetSsid(const std::string& ssid) { state_.ssid = ssid; }
    void SetIpAddress(const std::string& ip_address) { state_.ip_address = ip_address; }

private:
    NetworkState state_{true, "Simulated-Network", "192.168.1.100"};
};

}  // namespace homedeck
