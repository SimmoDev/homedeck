#pragma once

namespace homedeck {

// The reconnect-attempt bookkeeping half of wifi_setup.cpp's
// WIFI_EVENT_STA_DISCONNECTED handler, pulled out on its own so this one
// piece of real decision logic is host-testable - wifi_setup.cpp itself
// is 100% ESP-IDF-coupled (esp_wifi/esp_http_server/FreeRTOS) and
// unreachable from tests/, which is what let a real concurrency bug ship
// there undetected before a milestone-exit review caught it.
class WifiReconnectPolicy {
public:
    enum class Decision { kRetry, kGiveUp };

    explicit WifiReconnectPolicy(int max_setup_attempts) : max_setup_attempts_(max_setup_attempts) {}

    // `in_setup_mode` mirrors wifi_setup.cpp's own `setup_server !=
    // nullptr` - the give-up cap only applies while a freshly-submitted,
    // maybe-wrong set of credentials is still being tried during initial
    // setup. A normal post-setup reconnect to an already-trusted network
    // retries indefinitely instead, since giving up there would strand
    // the device with no Wi-Fi and no way back into setup mode.
    Decision OnDisconnected(bool in_setup_mode) {
        if (in_setup_mode && attempts_ >= max_setup_attempts_) {
            return Decision::kGiveUp;
        }
        ++attempts_;
        return Decision::kRetry;
    }

    // A freshly-submitted set of credentials always deserves its own
    // full set of attempts, per wifi_setup.cpp's ApplyWifiCredentials().
    void ResetAttempts() { attempts_ = 0; }

    int Attempts() const { return attempts_; }

private:
    int max_setup_attempts_;
    int attempts_ = 0;
};

}  // namespace homedeck
