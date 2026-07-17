#include "platform/task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <utility>

namespace homedeck {

namespace {

// Starting points, not deeply tuned - revisit if a real background task
// needs more headroom or contention becomes a problem.
constexpr uint32_t kStackSizeBytes = 4096;
constexpr UBaseType_t kPriority = tskIDLE_PRIORITY + 1;

// FreeRTOS's xTaskCreate only accepts a plain C function pointer plus a
// void* argument - unlike std::jthread (the host backend's primitive),
// it can't run an arbitrary capturing closure directly. TaskTrampoline
// bridges that gap, so it needs a type it can freely name from outside
// Task - Task::Impl itself is private and only accessible to Task's own
// member functions, so this context is a separate, ordinary struct that
// Impl merely owns a pointer to, not the same type.
struct TaskContext {
    std::stop_source stop_source;
    Task::Function function;
    // Given by the task right before it self-deletes, so the destructor
    // can block until the task has actually finished - not just been
    // asked to stop - matching std::jthread's join-on-destroy semantics
    // the host backend gets for free. FreeRTOS has no built-in "join"
    // for a self-deleting task.
    SemaphoreHandle_t finished = nullptr;
};

void TaskTrampoline(void* arg) {
    auto* context = static_cast<TaskContext*>(arg);
    context->function(context->stop_source.get_token());
    xSemaphoreGive(context->finished);
    vTaskDelete(nullptr);
}

}  // namespace

struct Task::Impl {
    std::unique_ptr<TaskContext> context = std::make_unique<TaskContext>();
};

Task::Task(const char* name, Function function) : impl_(std::make_unique<Impl>()) {
    impl_->context->function = std::move(function);
    impl_->context->finished = xSemaphoreCreateBinary();
    xTaskCreate(TaskTrampoline, name, kStackSizeBytes, impl_->context.get(), kPriority, nullptr);
}

Task::~Task() {
    impl_->context->stop_source.request_stop();
    xSemaphoreTake(impl_->context->finished, portMAX_DELAY);
    vSemaphoreDelete(impl_->context->finished);
}

}  // namespace homedeck
