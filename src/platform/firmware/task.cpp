#include "platform/task.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstdlib>
#include <utility>

namespace homedeck {

namespace {

// OpenMeteoWeatherProvider's poll Task (core/weather_provider.h) is the
// first Task consumer, and runs esp_http_client/TLS (mbedtls) plus
// JSON parsing - both genuinely stack-hungry, well past a minimal
// background task's usual footprint. Same "generous, not just past
// current requirement" reasoning as CONFIG_ESP_MAIN_TASK_STACK_SIZE/
// CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH in sdkconfig.defaults - PSRAM/
// RAM headroom makes a larger static stack cheap, and this constant
// applies uniformly to every Task instance, not just this one caller.
constexpr uint32_t kStackSizeBytes = 12288;
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
    // No meaningful degraded mode for either failure: a Task that's
    // silently never created leaves its caller believing background work
    // is running when it isn't, and ~Task()'s xSemaphoreTake() below would
    // block forever - no task would ever exist to give a null/unusable
    // semaphore. Matches AdminAuthService's identical "abort, don't limp
    // on" precedent for a broken platform primitive
    // (core/admin_auth_service.cpp's mbedtls_ctr_drbg_seed() check).
    if (impl_->context->finished == nullptr) {
        abort();
    }
    BaseType_t created = xTaskCreate(TaskTrampoline, name, kStackSizeBytes, impl_->context.get(), kPriority, nullptr);
    if (created != pdPASS) {
        abort();
    }
}

Task::~Task() {
    impl_->context->stop_source.request_stop();
    xSemaphoreTake(impl_->context->finished, portMAX_DELAY);
    vSemaphoreDelete(impl_->context->finished);
}

}  // namespace homedeck
