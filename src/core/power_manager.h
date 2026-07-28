#pragma once

#include "core/event_bus.h"
#include "platform/display_brightness.h"
#include "platform/time_source.h"
#include "platform/user_activity_source.h"

#include <chrono>
#include <optional>

namespace homedeck {

// See docs/architecture/power-management.md for the full model. Only
// kActive/kIdle are reachable via real transitions in this phase -
// kSleeping/kUpdating/kError exist in the type but have no trigger
// wired up yet.
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
    PowerManager(EventBus& event_bus, UserActivitySource& user_activity_source,
                 DisplayBrightness& display_brightness, TimeSource& time_source);

    PowerState State() const { return state_; }

    // Event-based, time-limited request to delay entering Sleeping - see
    // ADR-0005's sleep-veto decision. Not consulted by any real
    // transition yet (Sleeping is unreachable this phase) - built now
    // per ADR-0005's own reasoning that retrofitting it later would be
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
    std::optional<std::chrono::system_clock::time_point> sleep_veto_until_;
    EventBus::ScopedSubscription clock_subscription_;
};

}  // namespace homedeck
