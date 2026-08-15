#pragma once

#include <algorithm>
#include <chrono>

namespace homedeck {

// The generic exponential-backoff utility ADR-0006 already decided as
// the default for module reconnect logic ("a shared Core utility, with
// modules allowed to layer service-specific reconnection semantics on
// top") - nothing generic existed yet before this; WifiReconnectPolicy
// (wifi_reconnect_policy.h) is Wi-Fi-radio-specific (fixed interval, by
// its own documented design), not this. HarmonyConnection
// (harmony_connection.h) is the first real consumer.
//
// A pure decision object with no I/O, sleep, or Task/Timer of its own -
// same shape WifiReconnectPolicy already established - so it's directly
// host-testable without waiting out real delays.
class RetryBackoff {
public:
    // Doubles the delay after each failure, starting at initial_delay,
    // capped at max_delay. ResetAttempts() (a real successful connection)
    // starts the sequence over from initial_delay again.
    RetryBackoff(std::chrono::milliseconds initial_delay, std::chrono::milliseconds max_delay)
        : initial_delay_(initial_delay), max_delay_(max_delay) {}

    // The delay to wait before the next attempt, then bumps the
    // sequence forward - call once per failure, immediately before
    // waiting that long.
    std::chrono::milliseconds NextDelay() {
        std::chrono::milliseconds delay = current_delay_;
        current_delay_ = std::min(current_delay_ * 2, max_delay_);
        return delay;
    }

    void ResetAttempts() { current_delay_ = initial_delay_; }

private:
    std::chrono::milliseconds initial_delay_;
    std::chrono::milliseconds max_delay_;
    std::chrono::milliseconds current_delay_ = initial_delay_;
};

}  // namespace homedeck
