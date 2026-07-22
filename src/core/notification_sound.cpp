#include "core/notification_sound.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace homedeck {

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr double kDurationSeconds = 0.2;
constexpr double kToneHz = 880.0;
constexpr int kVolumePercent = 70;

// Generated, not a shipped audio asset - no audio asset exists anywhere
// in this repo, and one short tone doesn't justify introducing an asset
// pipeline. Duplicated rather than shared with simulator/main.cpp's own
// test-tone generator - small enough (a handful of lines) that a shared
// helper isn't worth it for the two places that need it.
std::vector<int16_t> GenerateTone() {
    std::vector<int16_t> tone(static_cast<size_t>(kSampleRate * kDurationSeconds));
    for (size_t i = 0; i < tone.size(); ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        tone[i] = static_cast<int16_t>(std::sin(2 * M_PI * kToneHz * t) * 20000);
    }
    return tone;
}

}  // namespace

NotificationSound::NotificationSound(EventBus& event_bus, AudioOutput& audio_output)
    : audio_output_(audio_output),
      subscription_(event_bus.Subscribe<NotificationEvent>([this](const NotificationEvent&) {
          // Both severities play the same tone - differentiating them
          // is real, unresolved product scope (nothing publishes
          // kAlertPriority yet), not something to design speculatively.
          {
              std::lock_guard<std::mutex> lock(wake_mutex_);
              pending_ = true;
          }
          wake_cv_.notify_one();
      })),
      sound_task_("notification-sound", [this](std::stop_token stop) { PlayLoop(stop); }) {}

void NotificationSound::PlayLoop(std::stop_token stop) {
    while (true) {
        std::unique_lock<std::mutex> lock(wake_mutex_);
        bool woke_for_pending = wake_cv_.wait(lock, stop, [this] { return pending_; });
        if (!woke_for_pending) {
            return;  // Stop requested, nothing left to play.
        }
        pending_ = false;
        lock.unlock();

        std::vector<int16_t> tone = GenerateTone();
        audio_output_.SetVolume(kVolumePercent);
        audio_output_.Play(tone.data(), tone.size(), kSampleRate);
    }
}

}  // namespace homedeck
