#include "platform/timer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

TEST(Timer, FiresRepeatedlyAtRoughlyThePeriod) {
    std::atomic<int> fire_count{0};
    homedeck::Timer timer("test-timer", std::chrono::milliseconds(20),
                           [&fire_count]() { ++fire_count; });
    std::this_thread::sleep_for(std::chrono::milliseconds(110));
    // ~5 fires expected in 110ms at a 20ms period; generous slack for
    // scheduling jitter on a loaded CI runner.
    EXPECT_GE(fire_count, 2);
}

TEST(Timer, DestructorStopsFiringPromptly) {
    std::atomic<int> fire_count{0};
    {
        homedeck::Timer timer("test-timer", std::chrono::milliseconds(10),
                               [&fire_count]() { ++fire_count; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    int count_at_destruction = fire_count;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fire_count, count_at_destruction);
}
