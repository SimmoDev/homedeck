#include "ui/activity_start_tracker.h"

namespace homedeck {

ActivityStartTracker::TapOutcome ActivityStartTracker::OnActivityTapped(const std::string& activity_id,
                                                                         const std::string& activity_label,
                                                                         bool is_current) {
    starting_activity_id_ = activity_id;
    command_failed_ = false;
    if (is_current) {
        return TapOutcome{"Resent to " + activity_label + ".", /*is_fresh_start=*/false};
    }
    return TapOutcome{"Starting " + activity_label + "...", /*is_fresh_start=*/true};
}

void ActivityStartTracker::ClearPending() {
    starting_activity_id_.clear();
    command_failed_ = false;
}

bool ActivityStartTracker::OnConnectionStateChanged(bool now_connected) {
    if (!now_connected && has_pending()) {
        starting_activity_id_.clear();
        return true;
    }
    if (now_connected && command_failed_) {
        command_failed_ = false;
    }
    return false;
}

std::optional<std::string> ActivityStartTracker::OnCommandDropped(const std::string& pending_activity_label) {
    if (!has_pending()) return std::nullopt;
    starting_activity_id_.clear();
    command_failed_ = true;
    return "Couldn't start " + pending_activity_label + " - hub unreachable";
}

std::optional<std::string> ActivityStartTracker::OnStartingTimedOut(const std::string& pending_activity_label) {
    if (!has_pending()) return std::nullopt;
    starting_activity_id_.clear();
    command_failed_ = true;
    return "No response starting " + pending_activity_label + " - try again";
}

}  // namespace homedeck
