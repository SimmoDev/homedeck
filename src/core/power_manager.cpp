#include "core/power_manager.h"

#include "core/clock.h"
#include "core/critical_battery_monitor.h"
#include "core/ota_routes.h"

#include <algorithm>

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
// A user-chosen Active brightness of 0% would be visually
// indistinguishable from Sleeping and unrecoverable without a hardware
// reset (nothing on screen to tap) - unlike kSleepBrightnessPercent,
// Active brightness is user-controlled, so it needs a floor Sleeping
// itself doesn't.
constexpr int kMinActiveBrightnessPercent = 5;

}  // namespace

PowerManager::PowerManager(EventBus& event_bus, UserActivitySource& user_activity_source,
                             DisplayBrightness& display_brightness, TimeSource& time_source,
                             int initial_active_brightness_percent)
    : event_bus_(event_bus),
      user_activity_source_(user_activity_source),
      display_brightness_(display_brightness),
      time_source_(time_source),
      active_brightness_percent_(
          std::clamp(initial_active_brightness_percent, kMinActiveBrightnessPercent, 100)) {
    clock_subscription_ = event_bus_.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent&) { OnTick(); });
    ota_subscription_ = event_bus_.SubscribeUi<OtaUpdateStateChangedEvent>(
        [this](const OtaUpdateStateChangedEvent& event) {
            if (event.in_progress) {
                // Guarded the same as the finishing branch below: kError
                // must always win over kUpdating (see the critical-battery
                // subscription's own comment), so a critical battery
                // already in kError before this fires must not be
                // clobbered back to kUpdating either.
                if (state_ != PowerState::kError) {
                    TransitionTo(PowerState::kUpdating);
                }
            } else if (state_ != PowerState::kError) {
                // Not unconditional: if a critical battery forced kError
                // during the write, the write finishing must not clobber
                // that back to kActive - the battery is still critical
                // regardless of how the OTA attempt ended.
                //
                // Not a bare TransitionTo(kActive) either - OnTick() was
                // suppressed for the whole write (see its own comment),
                // so a write that outlasted kIdleTimeoutMs would otherwise
                // flash to full Active brightness for one tick before the
                // very next OnTick() immediately dims/sleeps it again.
                // Land directly on the state inactivity already calls for.
                uint32_t inactive_ms = user_activity_source_.MillisecondsSinceLastActivity();
                if (inactive_ms >= kSleepTimeoutMs && !HasActiveSleepVeto()) {
                    TransitionTo(PowerState::kSleeping);
                } else if (inactive_ms >= kIdleTimeoutMs) {
                    TransitionTo(PowerState::kIdle);
                } else {
                    TransitionTo(PowerState::kActive);
                }
            }
        });
    critical_battery_subscription_ = event_bus_.SubscribeUi<CriticalBatteryStateChangedEvent>(
        [this](const CriticalBatteryStateChangedEvent& event) {
            if (event.critical) {
                // Unconditional, deliberately preempting kUpdating too -
                // see docs/architecture/power-management.md#status for
                // why an in-progress OTA write doesn't get priority here.
                TransitionTo(PowerState::kError);
            } else if (state_ == PowerState::kError) {
                TransitionTo(PowerState::kActive);
            }
        });
}

void PowerManager::OnTick() {
    // The Idle timeout doesn't apply while an OTA write is in progress
    // (see OtaUpdateStateChangedEvent's own comment for why) or while in
    // Error - leaving kError is exclusively the battery recovering, not
    // user activity.
    if (state_ == PowerState::kUpdating || state_ == PowerState::kError) {
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
    int brightness_percent = active_brightness_percent_;
    if (new_state == PowerState::kIdle) {
        // min(), not the bare constant - dimming must never end up
        // brighter than whatever the user chose for Active (e.g. an
        // Active brightness of 10% dimming "up" to kDimBrightnessPercent's
        // 20% would be backwards).
        brightness_percent = std::min(kDimBrightnessPercent, active_brightness_percent_);
    } else if (new_state == PowerState::kSleeping) {
        brightness_percent = kSleepBrightnessPercent;
    }
    display_brightness_.SetPercent(brightness_percent);
    event_bus_.Publish(PowerStateChangedEvent{new_state});
}

void PowerManager::SetActiveBrightnessPercent(int percent) {
    active_brightness_percent_ = std::clamp(percent, kMinActiveBrightnessPercent, 100);
    if (state_ == PowerState::kActive) {
        display_brightness_.SetPercent(active_brightness_percent_);
    }
}

void PowerManager::RequestSleepVeto(std::chrono::milliseconds duration) {
    sleep_veto_until_ = time_source_.Now() + duration;
}

bool PowerManager::HasActiveSleepVeto() const {
    return sleep_veto_until_.has_value() && time_source_.Now() < *sleep_veto_until_;
}

}  // namespace homedeck
