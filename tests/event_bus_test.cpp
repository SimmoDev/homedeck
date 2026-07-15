#include "core/event_bus.h"

#include <gtest/gtest.h>

namespace {

struct TestEvent {
    int value;
};

}  // namespace

TEST(EventBus, SubscribeDeliversPublishedPayload) {
    homedeck::EventBus bus;
    int received = 0;
    auto sub = bus.Subscribe<TestEvent>([&received](const TestEvent& e) { received = e.value; });
    bus.Publish(TestEvent{42});
    EXPECT_EQ(received, 42);
}

TEST(EventBus, UnsubscribeStopsDelivery) {
    homedeck::EventBus bus;
    int received = 0;
    {
        auto sub =
            bus.Subscribe<TestEvent>([&received](const TestEvent& e) { received = e.value; });
    }  // sub goes out of scope here, unsubscribing
    bus.Publish(TestEvent{42});
    EXPECT_EQ(received, 0);
}

TEST(EventBus, SubscribeUiRoutesThroughTheRegisteredDispatcher) {
    homedeck::EventBus bus;
    bool dispatcher_was_used = false;
    bus.SetUiDispatcher([&dispatcher_was_used](std::function<void()> fn) {
        dispatcher_was_used = true;
        fn();  // run inline for the test - a real UI task would defer via lv_async_call
    });

    int received = 0;
    auto sub =
        bus.SubscribeUi<TestEvent>([&received](const TestEvent& e) { received = e.value; });
    bus.Publish(TestEvent{7});

    EXPECT_TRUE(dispatcher_was_used);
    EXPECT_EQ(received, 7);
}

TEST(EventBus, DifferentEventTypesDoNotCrossDeliver) {
    struct OtherEvent {
        int value;
    };
    homedeck::EventBus bus;
    int test_event_received = 0;
    auto sub = bus.Subscribe<TestEvent>(
        [&test_event_received](const TestEvent& e) { test_event_received = e.value; });
    bus.Publish(OtherEvent{99});
    EXPECT_EQ(test_event_received, 0);
}
