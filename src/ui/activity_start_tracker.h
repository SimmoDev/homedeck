#pragma once

#include <optional>
#include <string>

namespace homedeck {

// Pure decision logic behind ActivitiesScreen's optimistic "Starting
// <name>..." status line - extracted out of activities_screen.cpp so it's
// host-testable without LVGL, the same "pull LVGL-adjacent pure logic out
// of src/ui/ specifically to make it testable" precedent text_format.h's
// SplitCamelCase() already established. Tracks exactly the two fields
// ActivitiesScreen itself used to hold directly: which activity (if any)
// is optimistically "pending" a hub-side confirmation, and whether the
// most recent attempt is already known to have failed. This class only
// decides state and hands back what status text to show (and whether a
// fresh start needs the timeout backstop armed) - it never touches LVGL
// itself; ActivitiesScreen applies every side effect (label text, timer
// arm/pause, button recoloring).
class ActivityStartTracker {
public:
    bool has_pending() const { return !starting_activity_id_.empty(); }
    bool command_failed() const { return command_failed_; }
    const std::string& pending_activity_id() const { return starting_activity_id_; }

    struct TapOutcome {
        std::string status_text;
        // True for a fresh start (arm starting_timeout_timer_), false for
        // a resend of the already-running activity (pause it instead - a
        // resend has no activity-ID change to ever clear itself with, so
        // it must not be timed out against an earlier fresh-start tap's
        // own schedule).
        bool is_fresh_start;
    };
    // A tap on `activity_id` (already-resolved display label
    // `activity_label`) - `is_current` distinguishes a fresh start from a
    // resend of the already-running activity.
    TapOutcome OnActivityTapped(const std::string& activity_id, const std::string& activity_label, bool is_current);

    // HarmonyCurrentActivityChangedEvent, or a mid-session config refresh
    // whose fresh activity list no longer contains the pending id -
    // either way, any pending wait/failure message is over.
    void ClearPending();

    // HarmonyConnectionStateChangedEvent - returns true if a pending tap
    // was just cleared by a disconnect (the caller should pause
    // starting_timeout_timer_ in that case, same as ClearPending()'s own
    // callers).
    bool OnConnectionStateChanged(bool now_connected);

    // HarmonyCommandDroppedEvent - std::nullopt if nothing was pending
    // (the caller's own no-op case), else the status text to show; either
    // way, any pending state is cleared.
    std::optional<std::string> OnCommandDropped(const std::string& pending_activity_label);

    // starting_timeout_timer_ fired - std::nullopt if already cleared by
    // some other event in the meantime, else the status text to show.
    std::optional<std::string> OnStartingTimedOut(const std::string& pending_activity_label);

private:
    std::string starting_activity_id_;
    bool command_failed_ = false;
};

}  // namespace homedeck
