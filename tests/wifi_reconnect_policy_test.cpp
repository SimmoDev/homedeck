#include "core/wifi_reconnect_policy.h"

#include <gtest/gtest.h>

TEST(WifiReconnectPolicyTest, RetriesInSetupModeUntilTheCapIsReached) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/3, /*normal_mode_recovery_attempts=*/100);

    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    // The 4th call is the first to observe attempts_ >= max_setup_attempts_.
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
}

TEST(WifiReconnectPolicyTest, GivingUpDoesNotIncrementFurther) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/1, /*normal_mode_recovery_attempts=*/100);

    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
    EXPECT_EQ(policy.OnDisconnected(true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
    EXPECT_EQ(policy.Attempts(), 1);
}

TEST(WifiReconnectPolicyTest, OutsideSetupModeRetriesIndefinitelyPastTheCap) {
    // A normal post-setup reconnect to an already-trusted network must
    // never give up - stranding the device with no Wi-Fi and no way back
    // into setup mode would be worse than retrying forever. It gets a
    // recovery access point instead - see the ShouldOfferRecovery tests
    // below - without this ever returning kGiveUp.
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2, /*normal_mode_recovery_attempts=*/100);

    for (int i = 0; i < 150; ++i) {
        EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    }
    EXPECT_EQ(policy.Attempts(), 150);
}

TEST(WifiReconnectPolicyTest, ResetAttemptsGivesAFreshSubmissionItsOwnFullCap) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/1, /*normal_mode_recovery_attempts=*/100);

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
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2, /*normal_mode_recovery_attempts=*/100);

    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/false), homedeck::WifiReconnectPolicy::Decision::kRetry);
    EXPECT_EQ(policy.OnDisconnected(/*in_setup_mode=*/true), homedeck::WifiReconnectPolicy::Decision::kGiveUp);
}

TEST(WifiReconnectPolicyTest, OffersRecoveryExactlyOnceWhenTheNormalModeThresholdIsCrossed) {
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2, /*normal_mode_recovery_attempts=*/3);

    for (int i = 0; i < 2; ++i) {
        policy.OnDisconnected(/*in_setup_mode=*/false);
        EXPECT_FALSE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));
    }
    policy.OnDisconnected(/*in_setup_mode=*/false);
    EXPECT_TRUE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));

    // Doesn't keep re-firing on every subsequent disconnect past the
    // threshold - wifi_setup.cpp's caller only needs to be told once to
    // bring the recovery access point up.
    policy.OnDisconnected(/*in_setup_mode=*/false);
    EXPECT_FALSE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));
}

TEST(WifiReconnectPolicyTest, NeverOffersRecoveryWhileInSetupMode) {
    // The initial no-stored-credentials setup flow already has its own
    // access point and form up; ShouldOfferRecovery exists for the
    // separate normal-mode case, where nothing is up yet.
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/100, /*normal_mode_recovery_attempts=*/3);

    for (int i = 0; i < 5; ++i) {
        policy.OnDisconnected(/*in_setup_mode=*/true);
        EXPECT_FALSE(policy.ShouldOfferRecovery(/*in_setup_mode=*/true));
    }
}

TEST(WifiReconnectPolicyTest, ResetAttemptsAllowsRecoveryToBeOfferedAgainAfterALaterOutage) {
    // A device that reconnects successfully (wifi_setup.cpp's
    // WIFI_EVENT_STA_GOT_IP handler calls ResetAttempts() on success, the
    // same as a fresh credential submission does) and later disconnects
    // for a second, separate long outage should get offered recovery
    // again, not just once per the device's entire uptime.
    homedeck::WifiReconnectPolicy policy(/*max_setup_attempts=*/2, /*normal_mode_recovery_attempts=*/2);

    policy.OnDisconnected(/*in_setup_mode=*/false);
    policy.OnDisconnected(/*in_setup_mode=*/false);
    EXPECT_TRUE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));

    policy.ResetAttempts();

    policy.OnDisconnected(/*in_setup_mode=*/false);
    EXPECT_FALSE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));
    policy.OnDisconnected(/*in_setup_mode=*/false);
    EXPECT_TRUE(policy.ShouldOfferRecovery(/*in_setup_mode=*/false));
}
