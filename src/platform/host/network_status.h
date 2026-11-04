#pragma once

#include "platform/network_status.h"

namespace homedeck {

// A mock value, toggled by the simulator's "toggle wifi disconnected" debug
// button - defaults to connected with plausible-looking SSID/IP, since most
// UI development wants a normal "online" dashboard, not a disconnected one.
class HostNetworkStatus : public NetworkStatus {
public:
    NetworkState Snapshot() const override { return state_; }

    void SetConnected(bool connected) { state_.connected = connected; }

private:
    NetworkState state_{true, "Simulated-Network", "192.168.1.100"};
};

}  // namespace homedeck
