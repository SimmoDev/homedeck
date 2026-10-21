#pragma once

#include "core/wifi_credentials.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace homedeck {

struct WifiCredentials {
    std::string ssid;
    std::string password;
};

// Holds the one Wi-Fi credential change a recovery-page visitor has asked
// for until someone at the device approves or rejects it. The recovery
// access point is open and its form is unauthenticated, so a submission
// there is only a request: it is applied when ApproveRecoveryCredentials()
// (wifi_setup.cpp) takes it after a tap on the device's own screen. A newer
// submission replaces an older one, so at most one request is ever pending.
// Each request has an id, and Take() requires the id the user was shown, so
// a submission that lands between the prompt and the tap cannot be approved
// in place of the request on screen.
// Thread-safe: the HTTP worker submits while the UI task takes or clears.
class PendingWifiCredentials {
public:
    // The new request's id, or nullopt (leaving any earlier request
    // untouched) if the pair would be rejected by IsValidWifiCredentials().
    std::optional<uint32_t> Submit(std::string ssid, std::string password) {
        if (!IsValidWifiCredentials(ssid, password)) {
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = WifiCredentials{std::move(ssid), std::move(password)};
        return ++last_id_;
    }

    // Removes and returns the pending request if it is the one with `id`.
    // A request that has since been replaced stays pending.
    std::optional<WifiCredentials> Take(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_.has_value() || id != last_id_) {
            return std::nullopt;
        }
        std::optional<WifiCredentials> taken = std::move(pending_);
        pending_.reset();
        return taken;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
    }

private:
    std::mutex mutex_;
    std::optional<WifiCredentials> pending_;
    uint32_t last_id_ = 0;
};

}  // namespace homedeck
