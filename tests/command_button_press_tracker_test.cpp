#include "ui/command_button_press_tracker.h"

#include <gtest/gtest.h>

namespace {

using homedeck::CommandButtonPressTracker;
using Action = CommandButtonPressTracker::Action;

// Buttons are opaque identities to CommandButtonPressTracker (see its own
// header comment) - any distinct addresses work as stand-ins for the real
// lv_obj_t* pointers DevicesScreen passes in.
int kButtonA = 0;
int kButtonB = 0;

TEST(CommandButtonPressTrackerTest, PlainTapWithNoLongPressSendsPressAndRelease) {
    CommandButtonPressTracker tracker;
    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/false), Action::kPressAndRelease);
}

TEST(CommandButtonPressTrackerTest, ReleaseWhileScrollingWithNoPriorLongPressSendsNothing) {
    CommandButtonPressTracker tracker;
    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/true), Action::kNone)
        << "a drag/scroll that happened to start on the button must send nothing";
}

TEST(CommandButtonPressTrackerTest, LongPressedAlwaysReturnsPress) {
    CommandButtonPressTracker tracker;
    EXPECT_EQ(tracker.OnLongPressed(&kButtonA), Action::kPress);
}

TEST(CommandButtonPressTrackerTest, LongPressRepeatAlwaysReturnsHold) {
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);
    EXPECT_EQ(tracker.OnLongPressRepeat(&kButtonA), Action::kHold);
    EXPECT_EQ(tracker.OnLongPressRepeat(&kButtonA), Action::kHold) << "fires repeatedly while held";
}

TEST(CommandButtonPressTrackerTest, ReleaseAfterALongPressSendsReleaseNotPressAndRelease) {
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);
    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/false), Action::kRelease);
}

TEST(CommandButtonPressTrackerTest, ReleaseAfterALongPressStillSendsReleaseEvenWhileNowScrolling) {
    // A genuine long press that then drags into a scroll before lift must
    // still send the matching release - otherwise the device would think
    // the button is stuck down. This is the one case where is_scrolling
    // must NOT suppress the action.
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);
    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/true), Action::kRelease);
}

TEST(CommandButtonPressTrackerTest, ReleaseClearsTheLongPressMarkingSoANextTapIsAPlainTapAgain) {
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);
    tracker.OnReleased(&kButtonA, /*is_scrolling=*/false);

    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/false), Action::kPressAndRelease)
        << "the long-press marking must not leak into a later, unrelated release on the same button";
}

TEST(CommandButtonPressTrackerTest, TwoButtonsTrackIndependently) {
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);

    // kButtonB was never long-pressed - a plain tap on it must not be
    // affected by kButtonA's own still-active long-press state.
    EXPECT_EQ(tracker.OnReleased(&kButtonB, /*is_scrolling=*/false), Action::kPressAndRelease);
    // kButtonA is still genuinely mid-long-press.
    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/false), Action::kRelease);
}

TEST(CommandButtonPressTrackerTest, ResetClearsEveryTrackedButtonsLongPressState) {
    // Mirrors ShowDeviceDetail()'s own rebuild, which deletes every
    // command button and must not leave a stale "active" marking that a
    // future, unrelated button reusing the same address could
    // accidentally inherit.
    CommandButtonPressTracker tracker;
    tracker.OnLongPressed(&kButtonA);
    tracker.Reset();

    EXPECT_EQ(tracker.OnReleased(&kButtonA, /*is_scrolling=*/false), Action::kPressAndRelease);
}

}  // namespace
