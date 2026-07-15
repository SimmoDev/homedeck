#pragma once

#include <functional>
#include <memory>
#include <stop_token>

namespace homedeck {

// A named background task - see ADR-0002's Core Concurrency Abstraction
// decision. Core and module code must depend on this interface
// exclusively, never on std::thread or FreeRTOS APIs directly.
//
// The function receives a std::stop_token so it can cooperatively exit
// when the Task is destroyed; the destructor requests a stop and joins
// before returning.
class Task {
public:
    using Function = std::function<void(std::stop_token)>;

    Task(const char* name, Function function);
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace homedeck
