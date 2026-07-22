#pragma once

#include "core/event_bus.h"
#include "core/notification.h"
#include "platform/audio_output.h"
#include "platform/task.h"

#include <condition_variable>
#include <mutex>

namespace homedeck {

// Sound presentation for Core's notification service - see
// docs/architecture/ui.md#notification-presentation. Portable, like
// OpenMeteoWeatherProvider: built entirely on already-portable
// interfaces (EventBus&, AudioOutput&), so one implementation serves
// both targets.
class NotificationSound {
public:
    NotificationSound(EventBus& event_bus, AudioOutput& audio_output);

private:
    void PlayLoop(std::stop_token stop);

    AudioOutput& audio_output_;

    std::mutex wake_mutex_;
    // condition_variable_any, not plain condition_variable - needed for
    // the stop_token-aware wait(lock, stop, pred) overload PlayLoop()
    // uses, the same primitive Queue<T>::Pop(std::stop_token)
    // (platform/queue.h) and Logger::WorkerLoop() already rely on for
    // an identical "wait until triggered or stopped, no polling
    // interval to fall back on" shape. Unlike
    // OpenMeteoWeatherProvider::PollLoop() (which can get away with a
    // stop_callback on a plain condition_variable, since its
    // wait_for()'s timeout is a real correctness backstop and the
    // callback is only a "stop and join promptly" optimization on top
    // of that), this loop has no interval to fall back on - without
    // the stop_token-aware wait, Task::~Task()'s request_stop() would
    // never be observed and the destructor would block forever.
    std::condition_variable_any wake_cv_;
    bool pending_ = false;

    EventBus::ScopedSubscription subscription_;
    // Declared last, same reason OpenMeteoWeatherProvider::poll_task_
    // is - its thread starts running PlayLoop() immediately, which
    // touches every member above it.
    Task sound_task_;
};

}  // namespace homedeck
