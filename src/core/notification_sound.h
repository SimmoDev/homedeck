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
    NotificationSound(EventBus& event_bus, AudioOutput& audio_output, int initial_volume_percent);

    // Both guarded by wake_mutex_, not just pending_/volume_percent_
    // themselves - PlayLoop() reads volume_percent_ from its own
    // background Task thread (platform/task.h), while these are called
    // from the UI thread (the quick-settings panel's slider), a real
    // cross-thread access the existing wake_mutex_ already exists to
    // guard pending_ against.
    int VolumePercent() const;
    void SetVolumePercent(int percent);

private:
    void PlayLoop(std::stop_token stop);

    AudioOutput& audio_output_;

    // mutable - VolumePercent() is logically const but still needs to
    // take this lock, since volume_percent_ is written from the UI
    // thread and read from PlayLoop()'s background thread.
    mutable std::mutex wake_mutex_;
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
    // Both guarded by wake_mutex_. A notification arriving while playing_
    // is already true doesn't set pending_ - see the subscription
    // callback's own comment for why a second play would otherwise queue
    // behind the first instead of the "replace, not queue" contract
    // NotificationBanner/NotificationWidget already follow.
    bool playing_ = false;
    int volume_percent_;

    EventBus::ScopedSubscription subscription_;
    // Declared last, same reason OpenMeteoWeatherProvider::poll_task_
    // is - its thread starts running PlayLoop() immediately, which
    // touches every member above it.
    Task sound_task_;
};

}  // namespace homedeck
