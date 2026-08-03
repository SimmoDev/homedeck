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

    WifiReconnectPolicy(int max_setup_attempts, int normal_mode_recovery_attempts)
        : max_setup_attempts_(max_setup_attempts), normal_mode_recovery_attempts_(normal_mode_recovery_attempts) {}

    // `in_setup_mode` mirrors wifi_setup.cpp's own `state.initial_provisioning` -
    // the give-up cap only applies while a freshly-submitted, maybe-wrong
    // set of credentials is still being tried during the device's very
    // first, no-stored-credentials setup flow. A normal post-setup
    // reconnect to an already-trusted network retries indefinitely
    // instead, since giving up there would strand the device with no
    // Wi-Fi and no way back into setup mode - see ShouldOfferRecovery()
    // below for how it gets one anyway, without ever actually giving up
    // the underlying retry loop.
    Decision OnDisconnected(bool in_setup_mode) {
        if (in_setup_mode && attempts_ >= max_setup_attempts_) {
            return Decision::kGiveUp;
        }
        ++attempts_;
        return Decision::kRetry;
    }

    // True exactly once - the first normal-mode (non-setup) disconnect
    // whose accrued attempt count crosses normal_mode_recovery_attempts_.
    // Deliberately independent of Decision above: a normal-mode reconnect
    // never gives up (OnDisconnected() always returns kRetry there), so
    // silently retrying with no way for the user to intervene could
    // otherwise continue indefinitely if the stored network is genuinely
    // gone for good (moved house, router replaced) rather than just
    // briefly down. This signals "also start offering a way back in
    // (recovery access point, alongside the continuing retries)," not
    // "stop retrying" - wifi_setup.cpp's own caller is what actually
    // brings up that access point once this returns true.
    bool ShouldOfferRecovery(bool in_setup_mode) const {
        return !in_setup_mode && attempts_ == normal_mode_recovery_attempts_;
    }

    // A freshly-submitted set of credentials, or a real successful
    // connection, always deserves its own full set of attempts before
    // either the setup-mode cap or the recovery threshold applies again -
    // see wifi_setup.cpp's ApplyWifiCredentials() and its
    // WIFI_EVENT_STA_GOT_IP handler, both of which call this.
    void ResetAttempts() { attempts_ = 0; }

    int Attempts() const { return attempts_; }

private:
    int max_setup_attempts_;
    int normal_mode_recovery_attempts_;
    int attempts_ = 0;
};

}  // namespace homedeck
