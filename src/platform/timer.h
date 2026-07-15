#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace homedeck {

// Periodic scheduled callback execution - see ADR-0002's Core
// Concurrency Abstraction decision. Core and module code must depend on
// this interface exclusively, never std::chrono/FreeRTOS timer APIs
// directly.
class Timer {
public:
    using Callback = std::function<void()>;

    // Fires `callback` every `period` until destroyed.
    Timer(const char* name, std::chrono::milliseconds period, Callback callback);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace homedeck
