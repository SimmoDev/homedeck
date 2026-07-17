#include "platform/task.h"

#include <thread>
#include <utility>

namespace homedeck {

struct Task::Impl {
    std::jthread thread;
};

Task::Task(const char* name, Function function) : impl_(std::make_unique<Impl>()) {
    // The name isn't used on the host backend - kept in the constructor
    // signature because the firmware/FreeRTOS backend
    // (src/platform/firmware/task.cpp) uses it for xTaskCreate's
    // required name parameter.
    (void)name;
    impl_->thread = std::jthread(
        [function = std::move(function)](std::stop_token token) { function(token); });
}

Task::~Task() = default;

}  // namespace homedeck
