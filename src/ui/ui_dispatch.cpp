#include "ui/ui_dispatch.h"

#include "lvgl.h"

#include <utility>

namespace homedeck {

namespace {

// Wraps the std::function in a heap allocation matching lv_async_call's
// single void* user_data slot; freed here once invoked.
void RunAndDelete(void* user_data) {
    auto* fn = static_cast<std::function<void()>*>(user_data);
    (*fn)();
    delete fn;
}

}  // namespace

void PostToUiThread(std::function<void()> fn) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
    lv_async_call(RunAndDelete, heap_fn);
}

}  // namespace homedeck
