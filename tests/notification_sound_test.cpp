#include "core/notification_sound.h"

#include "core/notification.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {

// Records Play() calls and signals a condition variable so the test can
// wait for NotificationSound's background Task to actually invoke it,
// rather than a blind sleep - the same synchronization need
// weather_provider_test.cpp's TriggerPoll test has for its own
// background Task.
class FakeAudioOutput : public homedeck::AudioOutput {
public:
    bool Play(const int16_t*, size_t, uint32_t) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            play_count_++;
        }
        cv_.notify_one();
        return true;
    }

    void SetVolume(int) override {}

    bool WaitForPlayCount(int expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return play_count_ >= expected; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int play_count_ = 0;
};

}  // namespace

TEST(NotificationSound, PublishingNotificationEventPlaysASound) {
    homedeck::EventBus bus;
    FakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output);

    bus.Publish(homedeck::NotificationEvent{"Battery low", homedeck::NotificationSeverity::kDeferred});

    EXPECT_TRUE(audio_output.WaitForPlayCount(1, std::chrono::seconds(2)));
}
