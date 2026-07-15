#include "platform/host/battery_reader.h"

#include <gtest/gtest.h>

TEST(HostBatteryReader, ReturnsAValueInValidPercentRange) {
    homedeck::HostBatteryReader reader;
    int percent = reader.ReadPercent();
    EXPECT_GE(percent, 0);
    EXPECT_LE(percent, 100);
}
