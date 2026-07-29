#include "ui/time_format.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// FormatLocalTime() renders in the process's local timezone (see its own
// comment) - pinned to UTC here so the expected strings below are
// deterministic regardless of the machine/CI environment's own zone.
class TimeFormatTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* existing = std::getenv("TZ");
        had_previous_tz_ = existing != nullptr;
        if (had_previous_tz_) previous_tz_ = existing;
        setenv("TZ", "UTC", 1);
        tzset();
    }

    void TearDown() override {
        if (had_previous_tz_) {
            setenv("TZ", previous_tz_.c_str(), 1);
        } else {
            unsetenv("TZ");
        }
        tzset();
    }

    // 2023-11-15 09:00:00 UTC, a Wednesday - confirmed via `date -u -d
    // @1700038800`.
    std::chrono::system_clock::time_point sample_time_ =
        std::chrono::system_clock::from_time_t(1700038800);

    bool had_previous_tz_ = false;
    std::string previous_tz_;
};

}  // namespace

TEST_F(TimeFormatTest, FormatsTimeOfDay) {
    char buffer[16] = {};
    homedeck::FormatLocalTime(sample_time_, "%H:%M", buffer, sizeof(buffer));
    EXPECT_STREQ(buffer, "09:00");
}

TEST_F(TimeFormatTest, FormatsDate) {
    char buffer[16] = {};
    homedeck::FormatLocalTime(sample_time_, "%a %d %b", buffer, sizeof(buffer));
    EXPECT_STREQ(buffer, "Wed 15 Nov");
}

// StatusBar's own format string - both halves in one call, not two
// separate FormatLocalTime() calls like ClockWidget's - confirms the
// function isn't hardcoded to a single format shape.
TEST_F(TimeFormatTest, FormatsCombinedTimeAndDate) {
    char buffer[32] = {};
    homedeck::FormatLocalTime(sample_time_, "%H:%M  %a %d %b", buffer, sizeof(buffer));
    EXPECT_STREQ(buffer, "09:00  Wed 15 Nov");
}

// strftime's exact content on a too-small buffer is implementation-
// defined (POSIX: "the contents of the array are indeterminate") - not
// something to assert on. What is guaranteed, and what every real call
// site depends on for safety, is that it never writes past the size
// given - verified here with guard bytes past the truncated region.
TEST_F(TimeFormatTest, NeverWritesPastTheGivenBufferSize) {
    char guarded[8];
    std::memset(guarded, 'G', sizeof(guarded));
    homedeck::FormatLocalTime(sample_time_, "%H:%M", guarded, 4);
    EXPECT_EQ(guarded[4], 'G');
    EXPECT_EQ(guarded[5], 'G');
    EXPECT_EQ(guarded[6], 'G');
    EXPECT_EQ(guarded[7], 'G');
}
