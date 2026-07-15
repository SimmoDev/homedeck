#include "core/clock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <thread>

namespace {

class FakeTimeSource : public homedeck::TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override { return fixed_time; }

    std::chrono::system_clock::time_point fixed_time =
        std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
};

}  // namespace

TEST(Clock, PublishesAnImmediateTickAtConstruction) {
    // Subscribe *before* constructing Clock - this is the scenario the
    // immediate-publish behavior exists for (see clock.cpp): a screen
    // subscribing and then being handed current time right away, not
    // left showing nothing until the first periodic tick.
    homedeck::EventBus bus;
    std::optional<homedeck::ClockTickEvent> received;
    auto sub = bus.Subscribe<homedeck::ClockTickEvent>(
        [&received](const homedeck::ClockTickEvent& e) { received = e; });

    FakeTimeSource time_source;
    // Long period - if this test passes, it's because of the immediate
    // publish, not a periodic tick arriving during the test.
    homedeck::Clock clock(time_source, bus, std::chrono::seconds(60));

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->time, time_source.fixed_time);
}

TEST(Clock, PublishesTickEventPeriodically) {
    homedeck::EventBus bus;
    FakeTimeSource time_source;
    // Short period so this test doesn't take a full second-plus to run.
    homedeck::Clock clock(time_source, bus, std::chrono::milliseconds(20));

    std::optional<homedeck::ClockTickEvent> received;
    auto sub = bus.Subscribe<homedeck::ClockTickEvent>(
        [&received](const homedeck::ClockTickEvent& e) { received = e; });

    for (int i = 0; i < 50 && !received.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->time, time_source.fixed_time);
}
