#pragma once

#include "core/event_bus.h"
#include "platform/display_brightness.h"
#include "platform/time_source.h"
#include "platform/user_activity_source.h"

#include <chrono>
#include <optional>

namespace homedeck {

// See docs/architecture/power-management.md for the full model. All
// five states are reachable via real transitions. kError is scoped
// narrowly to a critical-low-battery fault (see ADR-0005's Error-state-
// scope decision) - a charging fault and a thermal fault are permanent
// limitations of this board revision, not deferred work; neither has a
// battery-temperature or independent cable-presence signal anywhere in
// the design (see hardware.md#power).
enum class PowerState { kActive, kIdle, kSleeping, kUpdating, kError };

// Published on every real transition.
struct PowerStateChangedEvent {
    PowerState state;
};

// Central, Core-owned power state - see power-management.md's "every
// subsystem reads this one state" design principle (CLAUDE.md's "avoid
// scattered sleep logic"). Structural twin of LowBatteryMonitor/
// NetworkStatusMonitor (polls a portable interface on Clock's existing
// tick, publishes only on a real transition) with one difference:
// subscribes via SubscribeUi, not Subscribe, because - unlike those two
// - it touches LVGL indirectly through both UserActivitySource and
// DisplayBrightness's host backend, and Clock's tick never fires on the
// UI thread (see ADR-0011).
class PowerManager {
public:
    // A user-chosen Active brightness of 0% would be visually
    // indistinguishable from Sleeping and unrecoverable without a
    // hardware reset (nothing on screen to tap) - so Active brightness
    // has a floor Sleeping's own 0% doesn't need. Public so any UI
    // presenting a brightness control (QuickSettingsPanel) can bound its
    // own input range against this instead of duplicating the literal.
    static constexpr int kMinActiveBrightnessPercent = 5;

    PowerManager(EventBus& event_bus, UserActivitySource& user_activity_source,
                 DisplayBrightness& display_brightness, TimeSource& time_source,
                 int initial_active_brightness_percent);

    PowerState State() const { return state_; }

    int ActiveBrightnessPercent() const { return active_brightness_percent_; }

    // Sets the brightness TransitionTo() uses for kActive going forward
    // (replacing the previous baseline). Clamped to [kMinActiveBrightnessPercent,
    // 100], not [0, 100] - see the .cpp for why 0 isn't allowed. Applies
    // immediately via DisplayBrightness only if currently kActive -
    // Idle's dim/Sleeping's off stay untouched by the user's chosen "on"
    // brightness, the same way a phone still dims on timeout regardless
    // of your brightness setting.
    void SetActiveBrightnessPercent(int percent);

    // Event-based, time-limited request to delay entering Sleeping - see
    // ADR-0005's sleep-veto decision. HasActiveSleepVeto() is consulted
    // by OnTick()'s Idle->Sleeping transition; RequestSleepVeto() itself
    // still has no module caller until M3+, built ahead of one per
    // ADR-0005's own reasoning that retrofitting it later would be
    // disruptive to every module's background-task code. Each call
    // overwrites the previously recorded expiry rather than stacking
    // across repeated calls.
    void RequestSleepVeto(std::chrono::milliseconds duration);
    bool HasActiveSleepVeto() const;

private:
    void OnTick();
    void TransitionTo(PowerState new_state);

    EventBus& event_bus_;
    UserActivitySource& user_activity_source_;
    DisplayBrightness& display_brightness_;
    TimeSource& time_source_;
    PowerState state_ = PowerState::kActive;
    // Ground truth for whether an OTA write is actually in flight,
    // independent of state_ - a critical-battery event forces state_ to
    // kError even mid-write (see the critical-battery subscription's own
    // comment), which would otherwise lose the fact that kUpdating is
    // what recovery needs to land back on.
    bool ota_in_progress_ = false;
    int active_brightness_percent_;
    std::optional<std::chrono::system_clock::time_point> sleep_veto_until_;
    EventBus::ScopedSubscription clock_subscription_;
    EventBus::ScopedSubscription ota_subscription_;
    EventBus::ScopedSubscription critical_battery_subscription_;
};

}  // namespace homedeck
