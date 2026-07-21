#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace homedeck {

// A blocking, unbounded FIFO queue for cross-task handoff - see
// ADR-0002's Core Concurrency Abstraction decision.
//
// Header-only on the host backend (a template can't easily hide its
// implementation behind a pImpl the way Task/Timer do). The firmware/
// FreeRTOS specialization - likely needing trivially-copyable T given
// xQueueCreate's fixed-size semantics - isn't solved here; it's deferred
// to when firmware bring-up actually needs it. In practice this generic
// std::mutex/condition_variable_any implementation is now confirmed
// working on firmware too (see Logger, ADR-0020) - the same pthread-
// backed primitives AdminAuthService's mutex already relies on there -
// so a genuinely FreeRTOS-native backend (xQueueCreate) remains a
// possible future optimization, not a correctness requirement.
template <typename T>
class Queue {
public:
    Queue() = default;

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    void Push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            items_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    // Blocks until an item is available.
    T Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !items_.empty(); });
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Blocks until an item is available or stop_token is triggered -
    // returns nullopt in the latter case. Needs condition_variable_any
    // (not plain condition_variable), which is what supports a
    // stop_token-aware wait overload - see Logger::WorkerLoop() for the
    // caller this exists for: a background task that must be able to
    // wake and exit cleanly when Task's destructor requests a stop,
    // rather than staying blocked forever waiting for an item that will
    // never come.
    std::optional<T> Pop(std::stop_token stop_token) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool has_item = cv_.wait(lock, stop_token, [this] { return !items_.empty(); });
        if (!has_item) {
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

    // Non-blocking: returns immediately with nullopt if nothing is
    // currently available, rather than waiting for something to arrive.
    // Used to drain whatever else has already accumulated right after a
    // blocking Pop() wakes, coalescing a burst of near-simultaneous
    // pushes into one batch instead of processing them one at a time.
    std::optional<T> TryPop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (items_.empty()) {
            return std::nullopt;
        }
        T item = std::move(items_.front());
        items_.pop_front();
        return item;
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<T> items_;
};

}  // namespace homedeck
