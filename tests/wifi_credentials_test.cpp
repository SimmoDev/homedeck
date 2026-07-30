#include "core/wifi_credentials.h"

#include <gtest/gtest.h>

#include <string>

TEST(IsValidWifiCredentialsTest, AcceptsAnOrdinarySsidAndPassword) {
    EXPECT_TRUE(homedeck::IsValidWifiCredentials("MyNetwork", "hunter2"));
}

TEST(IsValidWifiCredentialsTest, AcceptsAnEmptyPassword) {
    // Open networks are a real case - password is optional, unlike ssid.
    EXPECT_TRUE(homedeck::IsValidWifiCredentials("MyOpenNetwork", ""));
}

TEST(IsValidWifiCredentialsTest, RejectsAnEmptySsid) {
    EXPECT_FALSE(homedeck::IsValidWifiCredentials("", "hunter2"));
}

TEST(IsValidWifiCredentialsTest, AcceptsAnSsidAtTheLengthLimit) {
    EXPECT_TRUE(homedeck::IsValidWifiCredentials(std::string(homedeck::kMaxWifiSsidBytes, 'a'), ""));
}

TEST(IsValidWifiCredentialsTest, RejectsAnSsidOverTheLengthLimit) {
    EXPECT_FALSE(homedeck::IsValidWifiCredentials(std::string(homedeck::kMaxWifiSsidBytes + 1, 'a'), ""));
}

TEST(IsValidWifiCredentialsTest, AcceptsAPasswordAtTheLengthLimit) {
    EXPECT_TRUE(homedeck::IsValidWifiCredentials("MyNetwork", std::string(homedeck::kMaxWifiPasswordBytes, 'a')));
}

TEST(IsValidWifiCredentialsTest, RejectsAPasswordOverTheLengthLimit) {
    EXPECT_FALSE(homedeck::IsValidWifiCredentials("MyNetwork", std::string(homedeck::kMaxWifiPasswordBytes + 1, 'a')));
}
