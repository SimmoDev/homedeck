#include "platform/task.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

TEST(Task, RunsTheGivenFunction) {
    std::atomic<bool> ran{false};
    {
        homedeck::Task task("test-task", [&ran](std::stop_token) { ran = true; });
        for (int i = 0; i < 100 && !ran; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_TRUE(ran);
}

TEST(Task, DestructorStopsAndJoinsPromptly) {
    std::atomic<bool> stop_seen{false};
    auto start = std::chrono::steady_clock::now();
    {
        homedeck::Task task("test-task", [&stop_seen](std::stop_token token) {
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            stop_seen = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(stop_seen);
    EXPECT_LT(elapsed, std::chrono::seconds(1));
}
