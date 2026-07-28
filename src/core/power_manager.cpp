#include "core/power_manager.h"

#include "core/clock.h"

namespace homedeck {

namespace {

// Provisional placeholders, not measured against real hardware - see
// docs/architecture/power-management.md's "every timing/threshold
// number in this document is a provisional placeholder" note.
constexpr uint32_t kIdleTimeoutMs = 30000;
constexpr int kDimBrightnessPercent = 20;

}  // namespace

PowerManager::PowerManager(EventBus& event_bus, UserActivitySource& user_activity_source,
                             DisplayBrightness& display_brightness, TimeSource& time_source)
    : event_bus_(event_bus),
      user_activity_source_(user_activity_source),
      display_brightness_(display_brightness),
      time_source_(time_source) {
    clock_subscription_ = event_bus_.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent&) { OnTick(); });
}

void PowerManager::OnTick() {
    uint32_t inactive_ms = user_activity_source_.MillisecondsSinceLastActivity();
    if (state_ == PowerState::kActive && inactive_ms >= kIdleTimeoutMs) {
        TransitionTo(PowerState::kIdle);
    } else if (state_ == PowerState::kIdle && inactive_ms < kIdleTimeoutMs) {
        TransitionTo(PowerState::kActive);
    }
}

void PowerManager::TransitionTo(PowerState new_state) {
    state_ = new_state;
    display_brightness_.SetPercent(new_state == PowerState::kIdle ? kDimBrightnessPercent : 100);
    event_bus_.Publish(PowerStateChangedEvent{new_state});
}

void PowerManager::RequestSleepVeto(std::chrono::milliseconds duration) {
    sleep_veto_until_ = time_source_.Now() + duration;
}

bool PowerManager::HasActiveSleepVeto() const {
    return sleep_veto_until_.has_value() && time_source_.Now() < *sleep_veto_until_;
}

}  // namespace homedeck
