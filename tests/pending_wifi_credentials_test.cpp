#include "core/pending_wifi_credentials.h"

#include <gtest/gtest.h>

#include <string>

TEST(PendingWifiCredentialsTest, StartsEmpty) {
    homedeck::PendingWifiCredentials pending;
    EXPECT_FALSE(pending.Take(1).has_value());
}

TEST(PendingWifiCredentialsTest, TakeReturnsTheSubmittedPairOnceAndEmptiesIt) {
    homedeck::PendingWifiCredentials pending;
    const auto id = pending.Submit("HomeNet", "hunter2hunter2");
    ASSERT_TRUE(id.has_value());

    const auto taken = pending.Take(*id);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ssid, "HomeNet");
    EXPECT_EQ(taken->password, "hunter2hunter2");
    EXPECT_FALSE(pending.Take(*id).has_value());
}

TEST(PendingWifiCredentialsTest, ANewerSubmissionReplacesTheOlderOneAndHasADifferentId) {
    homedeck::PendingWifiCredentials pending;
    const auto first = pending.Submit("First", "password-one");
    const auto second = pending.Submit("Second", "password-two");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(*first, *second);

    const auto taken = pending.Take(*second);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ssid, "Second");
}

// The user approved the request on screen; a submission that arrived after
// that prompt was drawn must not be applied by the same tap.
TEST(PendingWifiCredentialsTest, ApprovingAReplacedRequestAppliesNothingAndKeepsTheNewOne) {
    homedeck::PendingWifiCredentials pending;
    const auto shown = pending.Submit("Shown", "password-one");
    const auto newer = pending.Submit("Newer", "password-two");
    ASSERT_TRUE(shown.has_value());
    ASSERT_TRUE(newer.has_value());

    EXPECT_FALSE(pending.Take(*shown).has_value());

    const auto taken = pending.Take(*newer);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ssid, "Newer");
}

TEST(PendingWifiCredentialsTest, AnInvalidSubmissionIsRefusedAndKeepsTheEarlierRequest) {
    homedeck::PendingWifiCredentials pending;
    const auto kept = pending.Submit("Kept", "password-one");
    ASSERT_TRUE(kept.has_value());

    EXPECT_FALSE(pending.Submit("", "password").has_value());
    EXPECT_FALSE(pending.Submit(std::string(homedeck::kMaxWifiSsidBytes + 1, 's'), "password").has_value());
    EXPECT_FALSE(pending.Submit("Net", std::string(homedeck::kMaxWifiPasswordBytes + 1, 'p')).has_value());

    const auto taken = pending.Take(*kept);
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ssid, "Kept");
}

TEST(PendingWifiCredentialsTest, AnOpenNetworkWithNoPasswordIsAccepted) {
    homedeck::PendingWifiCredentials pending;
    EXPECT_TRUE(pending.Submit("CafeWifi", "").has_value());
}

TEST(PendingWifiCredentialsTest, ClearDropsThePendingRequest) {
    homedeck::PendingWifiCredentials pending;
    const auto id = pending.Submit("HomeNet", "password-one");
    ASSERT_TRUE(id.has_value());
    pending.Clear();
    EXPECT_FALSE(pending.Take(*id).has_value());
}
