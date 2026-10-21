#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>

namespace homedeck {

// The wake channel a module's connection loop sleeps on: one mutex and
// condition variable shared by a reconnect request ("trigger") and by
// whatever work other threads queue for the loop (commands, library
// requests). The module keeps those queues itself and guards them with
// Mutex(), so queuing work and waking the loop cannot interleave with the
// loop's check for pending work.
class ConnectionWake {
public:
    enum class Reason { kTimeout, kTriggered, kCommandPending, kStopRequested };

    // Asks the loop to re-resolve its target and reconnect.
    void Trigger() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requested_ = true;
        }
        cv_.notify_one();
    }

    // Wakes the loop after the caller has queued work under Mutex().
    void NotifyWorkQueued() { cv_.notify_one(); }

    // Guards the module's queues as well as the trigger flag.
    std::mutex& Mutex() { return mutex_; }

    // For a caller already holding Mutex(): drops a trigger that the
    // connection just established has already satisfied.
    void ClearTriggerLocked() { requested_ = false; }

    // Sleeps up to `delay`. `work_pending` is evaluated with Mutex() held and
    // may be empty when the caller wants only a trigger or a stop to wake
    // it (the loop has no connection to send queued work over). A trigger
    // takes precedence over pending work and is consumed by the wait.
    Reason Wait(std::chrono::milliseconds delay, std::stop_token stop, const std::function<bool()>& work_pending) {
        std::unique_lock<std::mutex> lock(mutex_);
        // _any for the stop_token-aware wait_for(): a stop request cannot be
        // missed between the predicate check and the block.
        cv_.wait_for(lock, stop, delay, [&] { return requested_ || (work_pending && work_pending()); });
        if (stop.stop_requested()) {
            return Reason::kStopRequested;
        }
        if (requested_) {
            requested_ = false;
            return Reason::kTriggered;
        }
        if (work_pending && work_pending()) {
            return Reason::kCommandPending;
        }
        return Reason::kTimeout;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool requested_ = false;
};

}  // namespace homedeck
