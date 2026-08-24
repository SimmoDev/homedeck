#include "ui/command_button_press_tracker.h"

namespace homedeck {

CommandButtonPressTracker::Action CommandButtonPressTracker::OnLongPressed(const void* button) {
    long_press_active_buttons_.insert(button);
    return Action::kPress;
}

CommandButtonPressTracker::Action CommandButtonPressTracker::OnLongPressRepeat(const void* /*button*/) const {
    return Action::kHold;
}

CommandButtonPressTracker::Action CommandButtonPressTracker::OnReleased(const void* button, bool is_scrolling) {
    if (long_press_active_buttons_.erase(button) > 0) {
        return Action::kRelease;
    }
    if (is_scrolling) {
        return Action::kNone;
    }
    return Action::kPressAndRelease;
}

void CommandButtonPressTracker::Reset() { long_press_active_buttons_.clear(); }

}  // namespace homedeck
