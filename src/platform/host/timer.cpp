#include "platform/timer.h"

#include "platform/task.h"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace homedeck {

struct Timer::Impl {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_ptr<Task> task;
};

Timer::Timer(const char* name, std::chrono::milliseconds period, Callback callback)
    : impl_(std::make_unique<Impl>()) {
    Impl* impl = impl_.get();
    impl->task = std::make_unique<Task>(
        name, [impl, period, callback = std::move(callback)](std::stop_token token) {
            std::unique_lock lock(impl->mutex);
            while (!token.stop_requested()) {
                // wait_for's stop_token overload wakes immediately on a
                // stop request rather than always waiting out the full
                // period - matters for destruction being prompt even
                // with a long period.
                impl->cv.wait_for(lock, token, period, [] { return false; });
                if (token.stop_requested()) break;
                lock.unlock();
                callback();
                lock.lock();
            }
        });
}

Timer::~Timer() = default;

}  // namespace homedeck
