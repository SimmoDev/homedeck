#include "core/retry_backoff.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(RetryBackoffTest, DoublesEachDelayUpToTheCap) {
    homedeck::RetryBackoff backoff(100ms, 800ms);

    EXPECT_EQ(backoff.NextDelay(), 100ms);
    EXPECT_EQ(backoff.NextDelay(), 200ms);
    EXPECT_EQ(backoff.NextDelay(), 400ms);
    EXPECT_EQ(backoff.NextDelay(), 800ms);
    // Stays at the cap rather than continuing to double past it.
    EXPECT_EQ(backoff.NextDelay(), 800ms);
    EXPECT_EQ(backoff.NextDelay(), 800ms);
}

TEST(RetryBackoffTest, ResetAttemptsStartsTheSequenceOver) {
    homedeck::RetryBackoff backoff(100ms, 800ms);

    EXPECT_EQ(backoff.NextDelay(), 100ms);
    EXPECT_EQ(backoff.NextDelay(), 200ms);

    backoff.ResetAttempts();

    EXPECT_EQ(backoff.NextDelay(), 100ms);
    EXPECT_EQ(backoff.NextDelay(), 200ms);
}

TEST(RetryBackoffTest, InitialDelayEqualToMaxDelayNeverGrows) {
    homedeck::RetryBackoff backoff(500ms, 500ms);

    EXPECT_EQ(backoff.NextDelay(), 500ms);
    EXPECT_EQ(backoff.NextDelay(), 500ms);
}
