#include "core/notification_sound.h"

#include "core/notification.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

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

    void SetVolume(int percent) override {
        std::lock_guard<std::mutex> lock(mutex_);
        last_volume_ = percent;
    }

    bool WaitForPlayCount(int expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, expected] { return play_count_ >= expected; });
    }

    int LastVolume() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_volume_;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int play_count_ = 0;
    int last_volume_ = -1;
};

// Like FakeAudioOutput, but Play() blocks until the test explicitly
// releases it - lets a test hold a play "in progress" open long enough to
// publish a second NotificationEvent into that window, the same real
// window a genuine ~200ms tone leaves open on real hardware.
class BlockingFakeAudioOutput : public homedeck::AudioOutput {
public:
    bool Play(const int16_t*, size_t, uint32_t) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            play_count_++;
        }
        started_cv_.notify_one();
        std::unique_lock<std::mutex> lock(mutex_);
        release_cv_.wait(lock, [this] { return released_; });
        return true;
    }

    void SetVolume(int) override {}

    bool WaitUntilPlayStarted(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return started_cv_.wait_for(lock, timeout, [this] { return play_count_ >= 1; });
    }

    void ReleasePlay() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        release_cv_.notify_one();
    }

    int PlayCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return play_count_;
    }

private:
    std::mutex mutex_;
    std::condition_variable started_cv_;
    std::condition_variable release_cv_;
    int play_count_ = 0;
    bool released_ = false;
};

}  // namespace

TEST(NotificationSound, PublishingNotificationEventPlaysASound) {
    homedeck::EventBus bus;
    FakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output, 70);

    bus.Publish(homedeck::NotificationEvent{"Battery low", homedeck::NotificationSeverity::kDeferred});

    EXPECT_TRUE(audio_output.WaitForPlayCount(1, std::chrono::seconds(2)));
}

TEST(NotificationSound, InitialVolumeIsClamped) {
    homedeck::EventBus bus;
    FakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output, 150);

    EXPECT_EQ(notification_sound.VolumePercent(), 100);
}

TEST(NotificationSound, SetVolumePercentClampsAndIsReadableBack) {
    homedeck::EventBus bus;
    FakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output, 70);

    notification_sound.SetVolumePercent(-10);
    EXPECT_EQ(notification_sound.VolumePercent(), 0);

    notification_sound.SetVolumePercent(55);
    EXPECT_EQ(notification_sound.VolumePercent(), 55);
}

TEST(NotificationSound, PlayAppliesTheCurrentlySetVolume) {
    homedeck::EventBus bus;
    FakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output, 70);

    notification_sound.SetVolumePercent(33);
    bus.Publish(homedeck::NotificationEvent{"Battery low", homedeck::NotificationSeverity::kDeferred});

    ASSERT_TRUE(audio_output.WaitForPlayCount(1, std::chrono::seconds(2)));
    EXPECT_EQ(audio_output.LastVolume(), 33);
}

TEST(NotificationSound, DoesNotQueueASecondPlayWhileOneIsAlreadyInProgress) {
    homedeck::EventBus bus;
    BlockingFakeAudioOutput audio_output;
    homedeck::NotificationSound notification_sound(bus, audio_output, 70);

    bus.Publish(homedeck::NotificationEvent{"Battery low", homedeck::NotificationSeverity::kDeferred});
    ASSERT_TRUE(audio_output.WaitUntilPlayStarted(std::chrono::seconds(2)));

    // A second notification arrives while the first tone is still
    // presenting - it must not queue a follow-up play once the first one
    // finishes (every tone is identical, so a queued echo would add
    // nothing - see NotificationSound's "replace, not queue" contract).
    bus.Publish(homedeck::NotificationEvent{"Battery low", homedeck::NotificationSeverity::kDeferred});
    audio_output.ReleasePlay();

    // Give the background Task a real window to have wrongly started a
    // second play, if this regressed.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(audio_output.PlayCount(), 1);
}
