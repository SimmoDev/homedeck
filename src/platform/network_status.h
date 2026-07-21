#pragma once

#include <string>

namespace homedeck {

// A snapshot rather than three loose getters - avoids a torn read
// across connected/ssid/ip_address (a caller seeing one field updated
// but not the others), which matters once a consumer wants all three
// together (the future network-status widget/Web UI page), even though
// today's only consumer (StatusBar) only reads `connected`.
struct NetworkState {
    bool connected;
    std::string ssid;
    std::string ip_address;
};

// Wi-Fi connectivity state - see docs/architecture/networking.md's
// "Connectivity status, exposed as Core state/events" responsibility.
// Small and virtual for the same reason as BatteryReader: a simple,
// rarely-called data reader, not a performance-sensitive primitive, and
// mockable matters directly. No EventBus dependency here by design -
// see NetworkStatusMonitor (src/core/network_status_monitor.h) for the
// latch-and-publish layer built on top of this.
//
// Deliberately excludes signal strength/RSSI - every esp_wifi_* call on
// this hardware is proxied over SDIO to the ESP32-C6 co-processor (see
// hardware.md's Wireless section), so reading it would mean a new RPC
// round trip on every poll, into a link with a documented RPC-timeout
// risk, for a number nothing currently needs.
class NetworkStatus {
public:
    virtual ~NetworkStatus() = default;

    virtual NetworkState Snapshot() const = 0;
};

}  // namespace homedeck
