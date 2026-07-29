#include "core/wifi_reconnect_policy.h"

#include <gtest/gtest.h>

TEST(WifiReconnectPolicyTest, RetriesInSetupModeUntilTheCapIsReached) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/3);

    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    // The 4th call is the first to observe attempts_ >= max_setup_attempts_.
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
}

TEST(WifiReconnectPolicyTest, GivingUpDoesNotIncrementFurther) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/1);

    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
    EXPECT_EQ(policy.Attempts(), 1);
}

TEST(WifiReconnectPolicyTest, OutsideSetupModeRetriesIndefinitelyPastTheCap) {
    // A normal post-setup reconnect to an already-trusted network must
    // never give up - stranding the device with no Wi-Fi and no way back
    // into setup mode would be worse than retrying forever.
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2);

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    }
    EXPECT_EQ(policy.Attempts(), 10);
}

TEST(WifiReconnectPolicyTest, ResetAttemptsGivesAFreshSubmissionItsOwnFullCap) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/1);

    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);

    policy.ResetAttempts();

    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
}

TEST(WifiReconnectPolicyTest, SwitchingIntoSetupModeAppliesTheCapToAttemptsAlreadyAccrued) {
    // A disconnect that happened before setup mode was ever entered (or
    // during a normal post-setup stretch) still counts toward the cap
    // the moment setup mode applies - attempts_ is one running counter,
    // not two separate ones per mode.
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2);

    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
}
