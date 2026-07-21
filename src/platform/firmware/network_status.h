#pragma once

#include "platform/network_status.h"

#include <mutex>

namespace homedeck {

// Backs NetworkStatus with real connection state, written by
// firmware/main/wifi_setup.cpp's own ESP-IDF WIFI_EVENT/IP_EVENT
// handler via SetConnectionState() - not derived from a fresh
// esp_wifi_* call on each Snapshot(), which would mean a new
// esp_wifi_remote RPC round trip to the ESP32-C6 co-processor on every
// read (see hardware.md's Wireless section for the documented
// RPC-timeout risk this avoids).
//
// Single-writer (only OnEvent(), always on the ESP-IDF system
// event-loop task) / multi-reader (the LVGL UI task today, httpd
// worker threads once a Web UI Wi-Fi page exists) - simpler than
// AdminAuthService's genuinely concurrent-writer case, but still a real
// cross-task race without the mutex.
class FirmwareNetworkStatus : public NetworkStatus {
public:
    NetworkState Snapshot() const override;

    // Not part of NetworkStatus - called by wifi_setup.cpp's event
    // handler on a real transition, never polled speculatively.
    void SetConnectionState(bool connected, const std::string& ssid, const std::string& ip_address);

private:
    mutable std::mutex mutex_;
    NetworkState state_{false, "", ""};
};

}  // namespace homedeck
