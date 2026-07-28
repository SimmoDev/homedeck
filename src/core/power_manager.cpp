#include "core/power_manager.h"

#include "core/clock.h"
#include "core/ota_routes.h"

namespace homedeck {

namespace {

// Provisional placeholders, not measured against real hardware - see
// docs/architecture/power-management.md's "every timing/threshold
// number in this document is a provisional placeholder" note.
constexpr uint32_t kIdleTimeoutMs = 30000;
// Cumulative inactivity, the same counter kIdleTimeoutMs above gates -
// built as an additional delay past the Idle threshold (not a bare
// standalone value) so retuning kIdleTimeoutMs can't silently change
// how long total inactivity before Sleeping takes too.
constexpr uint32_t kSleepTimeoutMs = kIdleTimeoutMs + 90000;
constexpr int kDimBrightnessPercent = 20;
// Not a placeholder like the two timeouts/kDimBrightnessPercent above -
// 0% (backlight fully off) is definitionally what Sleeping means, not a
// value to be tuned against hardware measurements.
constexpr int kSleepBrightnessPercent = 0;

}  // namespace

PowerManager::PowerManager(EventBus& event_bus, UserActivitySource& user_activity_source,
                             DisplayBrightness& display_brightness, TimeSource& time_source)
    : event_bus_(event_bus),
      user_activity_source_(user_activity_source),
      display_brightness_(display_brightness),
      time_source_(time_source) {
    clock_subscription_ = event_bus_.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent&) { OnTick(); });
    ota_subscription_ = event_bus_.SubscribeUi<OtaUpdateStateChangedEvent>(
        [this](const OtaUpdateStateChangedEvent& event) {
            TransitionTo(event.in_progress ? PowerState::kUpdating : PowerState::kActive);
        });
}

void PowerManager::OnTick() {
    // The Idle timeout doesn't apply while an OTA write is in progress -
    // see OtaUpdateStateChangedEvent's own comment for why.
    if (state_ == PowerState::kUpdating) {
        return;
    }
    uint32_t inactive_ms = user_activity_source_.MillisecondsSinceLastActivity();
    if (state_ == PowerState::kActive && inactive_ms >= kIdleTimeoutMs) {
        TransitionTo(PowerState::kIdle);
    } else if (state_ == PowerState::kIdle) {
        if (inactive_ms < kIdleTimeoutMs) {
            TransitionTo(PowerState::kActive);
        } else if (inactive_ms >= kSleepTimeoutMs && !HasActiveSleepVeto()) {
            TransitionTo(PowerState::kSleeping);
        }
    } else if (state_ == PowerState::kSleeping && inactive_ms < kIdleTimeoutMs) {
        // Same threshold Idle->Active already wakes on - inactive_ms is
        // one shared counter only real activity resets, so one bar is
        // correct for waking from either low-power state; this also
        // means a direct Sleeping->Active wake (skipping Idle) falls out
        // naturally, no special-casing needed.
        TransitionTo(PowerState::kActive);
    }
}

void PowerManager::TransitionTo(PowerState new_state) {
    state_ = new_state;
    int brightness_percent = 100;
    if (new_state == PowerState::kIdle) {
        brightness_percent = kDimBrightnessPercent;
    } else if (new_state == PowerState::kSleeping) {
        brightness_percent = kSleepBrightnessPercent;
    }
    display_brightness_.SetPercent(brightness_percent);
    event_bus_.Publish(PowerStateChangedEvent{new_state});
}

void PowerManager::RequestSleepVeto(std::chrono::milliseconds duration) {
    sleep_veto_until_ = time_source_.Now() + duration;
}

bool PowerManager::HasActiveSleepVeto() const {
    return sleep_veto_until_.has_value() && time_source_.Now() < *sleep_veto_until_;
}

}  // namespace homedeck
