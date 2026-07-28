#include "ui/ui_dispatch.h"

#include "lvgl.h"
#include "platform/queue.h"

#include <optional>
#include <utility>

namespace homedeck {

namespace {

// lv_async_call() - LVGL's own recommended cross-thread hand-off (see
// ADR-0011) - schedules each call as a fresh one-shot lv_timer, and
// lv_timer_create() inserts every new timer at the *head* of LVGL's
// internal timer list. Two calls made back-to-back therefore run in
// *reverse* order, not the order they were posted - a real bug for any
// "last write wins" UI update fed by multiple synchronous publishes
// (see docs/roadmap.md's Notifications item for the traced case this
// fixes). Queueing here ourselves and draining through a single
// recurring timer sidesteps that entirely: our own Queue<T> preserves
// real FIFO order regardless of how LVGL orders repeated timer
// creation.
Queue<std::function<void()>>& DispatchQueue() {
    static Queue<std::function<void()>> queue;
    return queue;
}

// Runs on the UI thread (invoked from lv_timer_handler()) - drains
// everything currently queued, in FIFO order, every time it fires.
void DrainQueue(lv_timer_t*) {
    while (std::optional<std::function<void()>> fn = DispatchQueue().TryPop()) {
        (*fn)();
    }
}

}  // namespace

void InitUiDispatchQueue() {
    lv_timer_t* timer = lv_timer_create(DrainQueue, /*period=*/0, /*user_data=*/nullptr);
    // Explicit even though -1 (infinite) is lv_timer_create's own
    // default - this timer must never behave like a one-shot
    // lv_async_call timer does.
    lv_timer_set_repeat_count(timer, -1);
}

void PostToUiThread(std::function<void()> fn) {
    // Pure application-level container operation, no LVGL interaction -
    // safe from any thread even before InitUiDispatchQueue() has run;
    // an item just waits in the queue until the drain timer exists.
    DispatchQueue().Push(std::move(fn));
}

}  // namespace homedeck
