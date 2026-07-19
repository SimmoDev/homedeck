#include "platform/host/battery_reader.h"

#include <gtest/gtest.h>

TEST(HostBatteryReader, ReturnsAValueInValidPercentRange) {
    homedeck::HostBatteryReader reader;
    int percent = reader.ReadPercent();
    EXPECT_GE(percent, 0);
    EXPECT_LE(percent, 100);
}

TEST(HostBatteryReader, SetPercentChangesTheReadValue) {
    homedeck::HostBatteryReader reader;
    reader.SetPercent(10);
    EXPECT_EQ(reader.ReadPercent(), 10);
}

TEST(HostBatteryReader, ExternalPowerConnectedDefaultsToFalseAndIsSettable) {
    homedeck::HostBatteryReader reader;
    EXPECT_FALSE(reader.IsExternalPowerConnected());

    reader.SetExternalPowerConnected(true);
    EXPECT_TRUE(reader.IsExternalPowerConnected());
}

TEST(HostBatteryReader, BatteryPresentDefaultsToTrueAndIsSettable) {
    homedeck::HostBatteryReader reader;
    EXPECT_TRUE(reader.IsBatteryPresent());

    reader.SetBatteryPresent(false);
    EXPECT_FALSE(reader.IsBatteryPresent());
}
