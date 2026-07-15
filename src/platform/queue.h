#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace homedeck {

// A blocking, unbounded FIFO queue for cross-task handoff - see
// ADR-0002's Core Concurrency Abstraction decision.
//
// Header-only on the host backend (a template can't easily hide its
// implementation behind a pImpl the way Task/Timer do). The firmware/
// FreeRTOS specialization - likely needing trivially-copyable T given
// xQueueCreate's fixed-size semantics - isn't solved here; it's deferred
// to when firmware bring-up actually needs it.
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

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<T> items_;
};

}  // namespace homedeck
