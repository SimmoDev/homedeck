#include "platform/queue.h"

#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <thread>

TEST(Queue, PushThenPopReturnsSameItem) {
    homedeck::Queue<int> queue;
    queue.Push(42);
    EXPECT_EQ(queue.Pop(), 42);
}

TEST(Queue, PreservesFifoOrder) {
    homedeck::Queue<int> queue;
    queue.Push(1);
    queue.Push(2);
    queue.Push(3);
    EXPECT_EQ(queue.Pop(), 1);
    EXPECT_EQ(queue.Pop(), 2);
    EXPECT_EQ(queue.Pop(), 3);
}

TEST(Queue, PopBlocksUntilAnItemIsPushed) {
    homedeck::Queue<int> queue;
    std::thread producer([&queue] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        queue.Push(7);
    });
    EXPECT_EQ(queue.Pop(), 7);  // blocks until the producer pushes
    producer.join();
}

TEST(Queue, TryPopReturnsNulloptWhenEmpty) {
    homedeck::Queue<int> queue;
    EXPECT_FALSE(queue.TryPop().has_value());
}

TEST(Queue, TryPopReturnsItemWithoutBlocking) {
    homedeck::Queue<int> queue;
    queue.Push(5);
    auto item = queue.TryPop();
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 5);
}

TEST(Queue, StopAwarePopReturnsItemWhenOneArrives) {
    homedeck::Queue<int> queue;
    queue.Push(9);
    std::stop_source stop_source;
    auto item = queue.Pop(stop_source.get_token());
    ASSERT_TRUE(item.has_value());
    EXPECT_EQ(*item, 9);
}

// The exact scenario Logger::WorkerLoop() relies on for clean shutdown -
// see ADR-0020: a background task blocked waiting for the next log entry
// must wake and exit when Task's destructor requests a stop, rather than
// blocking forever on an item that will never arrive.
TEST(Queue, StopAwarePopReturnsNulloptWhenStoppedBeforeAnItemArrives) {
    homedeck::Queue<int> queue;
    std::stop_source stop_source;
    std::thread stopper([&stop_source] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop_source.request_stop();
    });
    auto item = queue.Pop(stop_source.get_token());
    EXPECT_FALSE(item.has_value());
    stopper.join();
}
