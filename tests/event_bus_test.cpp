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

// A dispatcher that queues instead of running inline, standing in for
// lv_async_call()'s real "runs on the next tick, not now" behavior - the
// existing SubscribeUiRoutesThroughTheRegisteredDispatcher test above
// runs its dispatcher inline, which can never exercise the gap ADR-0011
// describes (Unsubscribe racing a still-queued deferred call).
struct QueuingDispatcher {
    std::vector<std::function<void()>> queued;

    void operator()(std::function<void()> fn) { queued.push_back(std::move(fn)); }

    void RunAllQueued() {
        auto to_run = std::move(queued);
        queued.clear();
        for (auto& fn : to_run) fn();
    }
};

TEST(EventBus, DeferredUiCallbackStillFiresWhenSubscriberOutlivesTheTick) {
    homedeck::EventBus bus;
    QueuingDispatcher dispatcher;
    bus.SetUiDispatcher([&dispatcher](std::function<void()> fn) { dispatcher(std::move(fn)); });

    int received = 0;
    auto sub =
        bus.SubscribeUi<TestEvent>([&received](const TestEvent& e) { received = e.value; });
    bus.Publish(TestEvent{7});
    ASSERT_EQ(received, 0);  // not yet delivered - still queued, matching the real async gap

    dispatcher.RunAllQueued();
    EXPECT_EQ(received, 7);
}

TEST(EventBus, DeferredUiCallbackIsSkippedIfUnsubscribedBeforeTheTickRuns) {
    homedeck::EventBus bus;
    QueuingDispatcher dispatcher;
    bus.SetUiDispatcher([&dispatcher](std::function<void()> fn) { dispatcher(std::move(fn)); });

    int received = 0;
    auto sub =
        bus.SubscribeUi<TestEvent>([&received](const TestEvent& e) { received = e.value; });
    bus.Publish(TestEvent{7});  // queues a deferred call, not yet run

    sub.Reset();  // unsubscribes - the subscriber may now be destroyed in real use

    // Simulates the LVGL tick that runs the deferred call firing only
    // after Unsubscribe already completed (see ADR-0011's "Resolved
    // (M2)" note) - liveness is re-checked at this point, so the stale
    // callback must not run.
    dispatcher.RunAllQueued();
    EXPECT_EQ(received, 0);
}
