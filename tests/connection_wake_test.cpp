#include "core/connection_wake.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

using homedeck::ConnectionWake;
using std::chrono::milliseconds;

constexpr milliseconds kShort{30};
constexpr milliseconds kLong{5000};

}  // namespace

TEST(ConnectionWakeTest, TimesOutWhenNothingHappens) {
    ConnectionWake wake;
    std::stop_source stop;
    EXPECT_EQ(wake.Wait(kShort, stop.get_token(), nullptr), ConnectionWake::Reason::kTimeout);
}

TEST(ConnectionWakeTest, ATriggerBeforeTheWaitIsConsumedOnce) {
    ConnectionWake wake;
    std::stop_source stop;
    wake.Trigger();
    EXPECT_EQ(wake.Wait(kLong, stop.get_token(), nullptr), ConnectionWake::Reason::kTriggered);
    EXPECT_EQ(wake.Wait(kShort, stop.get_token(), nullptr), ConnectionWake::Reason::kTimeout);
}

TEST(ConnectionWakeTest, PendingWorkWakesAWaitThatWatchesIt) {
    ConnectionWake wake;
    std::stop_source stop;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(wake.Mutex());
        queued = true;
    }
    wake.NotifyWorkQueued();
    EXPECT_EQ(wake.Wait(kLong, stop.get_token(), [&] { return queued; }), ConnectionWake::Reason::kCommandPending);
}

TEST(ConnectionWakeTest, PendingWorkIsIgnoredWhenTheCallerDoesNotWatchIt) {
    ConnectionWake wake;
    std::stop_source stop;
    {
        std::lock_guard<std::mutex> lock(wake.Mutex());
    }
    wake.NotifyWorkQueued();
    EXPECT_EQ(wake.Wait(kShort, stop.get_token(), nullptr), ConnectionWake::Reason::kTimeout);
}

TEST(ConnectionWakeTest, ATriggerWinsOverPendingWork) {
    ConnectionWake wake;
    std::stop_source stop;
    wake.Trigger();
    EXPECT_EQ(wake.Wait(kLong, stop.get_token(), [] { return true; }), ConnectionWake::Reason::kTriggered);
    EXPECT_EQ(wake.Wait(kLong, stop.get_token(), [] { return true; }), ConnectionWake::Reason::kCommandPending);
}

TEST(ConnectionWakeTest, ClearTriggerLockedDropsAnUnconsumedTrigger) {
    ConnectionWake wake;
    std::stop_source stop;
    wake.Trigger();
    {
        std::lock_guard<std::mutex> lock(wake.Mutex());
        wake.ClearTriggerLocked();
    }
    EXPECT_EQ(wake.Wait(kShort, stop.get_token(), nullptr), ConnectionWake::Reason::kTimeout);
}

TEST(ConnectionWakeTest, AStopRequestEndsTheWaitPromptly) {
    ConnectionWake wake;
    std::stop_source stop;
    std::atomic<ConnectionWake::Reason> result{ConnectionWake::Reason::kTimeout};
    std::thread waiter([&] { result = wake.Wait(kLong, stop.get_token(), nullptr); });
    std::this_thread::sleep_for(milliseconds(20));
    stop.request_stop();
    waiter.join();
    EXPECT_EQ(result.load(), ConnectionWake::Reason::kStopRequested);
}

TEST(ConnectionWakeTest, ATriggerFromAnotherThreadWakesTheWait) {
    ConnectionWake wake;
    std::stop_source stop;
    std::atomic<ConnectionWake::Reason> result{ConnectionWake::Reason::kTimeout};
    std::thread waiter([&] { result = wake.Wait(kLong, stop.get_token(), nullptr); });
    std::this_thread::sleep_for(milliseconds(20));
    wake.Trigger();
    waiter.join();
    EXPECT_EQ(result.load(), ConnectionWake::Reason::kTriggered);
}
