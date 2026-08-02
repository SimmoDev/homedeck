#include "core/ota_gate.h"

#include "platform/host/battery_reader.h"

#include <gtest/gtest.h>

// Only exercised indirectly (via specific battery percentages like 5/50/100)
// through ota_routes_test.cpp until now - this asserts the actual
// kMinBatteryPercent boundary EvaluateOtaGate() draws the line at, not just
// values comfortably on either side of it.

TEST(OtaGateTest, GateIsClosedOnePercentBelowTheThreshold) {
    homedeck::HostBatteryReader battery_reader;
    battery_reader.SetPercent(29);
    battery_reader.SetExternalPowerConnected(false);

    homedeck::OtaGateStatus status = homedeck::EvaluateOtaGate(battery_reader);
    EXPECT_FALSE(status.open);
    EXPECT_FALSE(status.reason.empty());
}

TEST(OtaGateTest, GateIsOpenExactlyAtTheThreshold) {
    homedeck::HostBatteryReader battery_reader;
    battery_reader.SetPercent(30);
    battery_reader.SetExternalPowerConnected(false);

    homedeck::OtaGateStatus status = homedeck::EvaluateOtaGate(battery_reader);
    EXPECT_TRUE(status.open);
    EXPECT_TRUE(status.reason.empty());
}

TEST(OtaGateTest, ExternalPowerOpensTheGateBelowTheThreshold) {
    homedeck::HostBatteryReader battery_reader;
    battery_reader.SetPercent(1);
    battery_reader.SetExternalPowerConnected(true);

    homedeck::OtaGateStatus status = homedeck::EvaluateOtaGate(battery_reader);
    EXPECT_TRUE(status.open);
}
