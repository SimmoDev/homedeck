#include "platform/queue.h"

#include <gtest/gtest.h>

#include <chrono>
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
