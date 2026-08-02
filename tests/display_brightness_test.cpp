#include "platform/display_brightness.h"

#include <gtest/gtest.h>

// ClampBrightnessPercent() is the one piece of DisplayBrightness's two
// implementations (Host/FirmwareDisplayBrightness) that's actually
// portable - both previously duplicated an identical, untested
// std::clamp(percent, 0, 100) inline; this exercises the shared version
// directly, which neither LVGL- nor ESP-IDF-coupled implementation could
// be tested through in this suite.

TEST(DisplayBrightnessTest, ClampsBelowZeroUpToZero) {
    EXPECT_EQ(homedeck::ClampBrightnessPercent(-5), 0);
}

TEST(DisplayBrightnessTest, ClampsAboveOneHundredDownToOneHundred) {
    EXPECT_EQ(homedeck::ClampBrightnessPercent(150), 100);
}

TEST(DisplayBrightnessTest, LeavesAnInRangeValueUnchanged) {
    EXPECT_EQ(homedeck::ClampBrightnessPercent(42), 42);
}

TEST(DisplayBrightnessTest, LeavesBothBoundariesUnchanged) {
    EXPECT_EQ(homedeck::ClampBrightnessPercent(0), 0);
    EXPECT_EQ(homedeck::ClampBrightnessPercent(100), 100);
}
