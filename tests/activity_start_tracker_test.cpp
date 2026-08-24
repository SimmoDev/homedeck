#include "ui/activity_start_tracker.h"

#include <gtest/gtest.h>

namespace {

using homedeck::ActivityStartTracker;

TEST(ActivityStartTrackerTest, FreshTapSetsPendingAndReturnsStartingText) {
    ActivityStartTracker tracker;
    auto outcome = tracker.OnActivityTapped("123", "Watch TV", /*is_current=*/false);
    EXPECT_EQ(outcome.status_text, "Starting Watch TV...");
    EXPECT_TRUE(outcome.is_fresh_start);
    EXPECT_TRUE(tracker.has_pending());
    EXPECT_EQ(tracker.pending_activity_id(), "123");
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, ResendOfTheCurrentActivityReturnsResentTextAndIsNotAFreshStart) {
    ActivityStartTracker tracker;
    auto outcome = tracker.OnActivityTapped("123", "Watch TV", /*is_current=*/true);
    EXPECT_EQ(outcome.status_text, "Resent to Watch TV.");
    EXPECT_FALSE(outcome.is_fresh_start);
    EXPECT_TRUE(tracker.has_pending()) << "a resend still tracks as pending, same as a fresh start";
}

TEST(ActivityStartTrackerTest, ANewTapClearsAnyPriorCommandFailedFlag) {
    ActivityStartTracker tracker;
    ASSERT_FALSE(tracker.OnCommandDropped("Watch TV").has_value())
        << "nothing pending yet - OnCommandDropped is a no-op";
    tracker.OnActivityTapped("1", "Watch TV", false);
    tracker.OnCommandDropped("Watch TV");
    ASSERT_TRUE(tracker.command_failed());

    tracker.OnActivityTapped("2", "Listen to Music", false);
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, CurrentActivityChangeClearsPendingRegardlessOfWhichActivity) {
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    tracker.ClearPending();
    EXPECT_FALSE(tracker.has_pending());
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, DisconnectWhilePendingClearsItAndReportsTrue) {
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    EXPECT_TRUE(tracker.OnConnectionStateChanged(/*now_connected=*/false));
    EXPECT_FALSE(tracker.has_pending());
}

TEST(ActivityStartTrackerTest, DisconnectWithNothingPendingReportsFalse) {
    ActivityStartTracker tracker;
    EXPECT_FALSE(tracker.OnConnectionStateChanged(/*now_connected=*/false));
}

TEST(ActivityStartTrackerTest, ReconnectingClearsAStandingCommandFailedFlag) {
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    tracker.OnCommandDropped("Watch TV");
    ASSERT_TRUE(tracker.command_failed());

    EXPECT_FALSE(tracker.OnConnectionStateChanged(/*now_connected=*/true))
        << "nothing was pending (it was already cleared by the drop above), so no timer-pause request";
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, ReconnectingWithNoFailureAndNothingPendingIsANoOp) {
    ActivityStartTracker tracker;
    EXPECT_FALSE(tracker.OnConnectionStateChanged(/*now_connected=*/true));
    EXPECT_FALSE(tracker.command_failed());
    EXPECT_FALSE(tracker.has_pending());
}

TEST(ActivityStartTrackerTest, CommandDroppedWithNothingPendingIsANoOp) {
    ActivityStartTracker tracker;
    EXPECT_FALSE(tracker.OnCommandDropped("Watch TV").has_value());
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, CommandDroppedWhilePendingClearsItSetsFailedAndReturnsMessage) {
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    auto text = tracker.OnCommandDropped("Watch TV");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "Couldn't start Watch TV - hub unreachable");
    EXPECT_FALSE(tracker.has_pending());
    EXPECT_TRUE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, StartingTimedOutWithNothingPendingIsANoOp) {
    ActivityStartTracker tracker;
    EXPECT_FALSE(tracker.OnStartingTimedOut("Watch TV").has_value());
}

TEST(ActivityStartTrackerTest, StartingTimedOutWhilePendingClearsItSetsFailedAndReturnsMessage) {
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    auto text = tracker.OnStartingTimedOut("Watch TV");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "No response starting Watch TV - try again");
    EXPECT_FALSE(tracker.has_pending());
    EXPECT_TRUE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, ActivityChangeAfterATimeoutAlreadyFiredIsHarmless) {
    // Pairwise ordering: OnStartingTimedOut() then a later
    // HarmonyCurrentActivityChangedEvent (ClearPending()) - the timeout
    // already cleared has_pending(), so ClearPending() must not do
    // anything surprising to command_failed_ that a fresh tap wouldn't
    // already reset on its own.
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    tracker.OnStartingTimedOut("Watch TV");
    ASSERT_TRUE(tracker.command_failed());

    tracker.ClearPending();
    EXPECT_FALSE(tracker.has_pending());
    EXPECT_FALSE(tracker.command_failed());
}

TEST(ActivityStartTrackerTest, DropThenDisconnectDoesNotDoubleClearOrCrash) {
    // Pairwise ordering: OnCommandDropped() then OnConnectionStateChanged()
    // observing the same already-cleared pending state.
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    tracker.OnCommandDropped("Watch TV");
    ASSERT_FALSE(tracker.has_pending());

    EXPECT_FALSE(tracker.OnConnectionStateChanged(/*now_connected=*/false))
        << "nothing pending anymore - the drop already cleared it, so no timer-pause request";
}

TEST(ActivityStartTrackerTest, NewTapAfterADisconnectClearReplacesThePendingActivity) {
    // Pairwise ordering: OnConnectionStateChanged(disconnect) then a new
    // OnActivityTapped() for a different activity - the new tap must win
    // cleanly, not be blocked or merged with the cleared one.
    ActivityStartTracker tracker;
    tracker.OnActivityTapped("123", "Watch TV", false);
    tracker.OnConnectionStateChanged(/*now_connected=*/false);
    ASSERT_FALSE(tracker.has_pending());

    auto outcome = tracker.OnActivityTapped("456", "Listen to Music", false);
    EXPECT_EQ(outcome.status_text, "Starting Listen to Music...");
    EXPECT_EQ(tracker.pending_activity_id(), "456");
}

}  // namespace
