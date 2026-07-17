#include "platform/timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include <utility>

namespace homedeck {

// Built directly on FreeRTOS's software timers (xTimerCreate), per
// ADR-0002's Core Concurrency Abstraction decision - not on Task, unlike
// the host backend. All FreeRTOS software timer callbacks run on one
// shared timer-service task regardless of how many Timers exist, which
// is the right tradeoff here: periodic callbacks in this codebase are
// lightweight (e.g. Clock publishing an event), so a dedicated task per
// Timer would just be wasted stack/RAM.

namespace {

// xTimerCreate's callback only accepts a TimerHandle_t, recovered back
// to context via pvTimerGetTimerID() - it can't run an arbitrary
// capturing closure. TimerTrampoline needs a type it can freely name
// from outside Timer; Timer::Impl itself is private and only accessible
// to Timer's own member functions, so this context is a separate,
// ordinary struct that Impl merely owns a pointer to, not the same type.
struct TimerContext {
    Timer::Callback callback;
    TimerHandle_t handle = nullptr;
};

void TimerTrampoline(TimerHandle_t handle) {
    auto* context = static_cast<TimerContext*>(pvTimerGetTimerID(handle));
    context->callback();
}

void GiveSemaphore(void* context, uint32_t /*unused*/) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(context));
}

}  // namespace

struct Timer::Impl {
    std::unique_ptr<TimerContext> context = std::make_unique<TimerContext>();
};

Timer::Timer(const char* name, std::chrono::milliseconds period, Callback callback)
    : impl_(std::make_unique<Impl>()) {
    impl_->context->callback = std::move(callback);
    impl_->context->handle = xTimerCreate(name, pdMS_TO_TICKS(period.count()), pdTRUE,
                                           impl_->context.get(), TimerTrampoline);
    xTimerStart(impl_->context->handle, portMAX_DELAY);
}

Timer::~Timer() {
    xTimerDelete(impl_->context->handle, portMAX_DELAY);
    // xTimerDelete returning only means the delete *command* was
    // successfully queued - not that FreeRTOS's timer-service task has
    // processed it yet. A callback already in flight, or about to fire,
    // could still touch impl_->context after this call returns and
    // before it's freed. All timer commands (start/stop/delete) and
    // pended function calls funnel through one queue processed strictly
    // in order by that single shared task, so queuing a pended call
    // right after the delete command guarantees - by the time it runs -
    // the delete ahead of it has already been fully processed, and no
    // callback for this timer can still be executing (the same one task
    // can't be doing both at once). Blocking on that pended call
    // finishing is what makes this destructor actually safe to return
    // from, not just "probably fine."
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    xTimerPendFunctionCall(GiveSemaphore, done, 0, portMAX_DELAY);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}

}  // namespace homedeck
