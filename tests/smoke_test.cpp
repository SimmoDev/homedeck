#include <gmock/gmock.h>
#include <gtest/gtest.h>

// Proves GoogleTest itself builds, links, and runs.
TEST(Smoke, GoogleTestRuns) {
    EXPECT_EQ(1 + 1, 2);
}

// Proves GoogleMock specifically works (its own linking requirements,
// distinct from GTest), using a minimal stand-in for the kind of
// hardware-abstraction interface Core will mock in real tests later
// (see docs/architecture/overview.md#hardware-abstraction).
class BatteryReader {
public:
    virtual ~BatteryReader() = default;
    virtual int ReadPercent() const = 0;
};

class MockBatteryReader : public BatteryReader {
public:
    MOCK_METHOD(int, ReadPercent, (), (const, override));
};

TEST(Smoke, GoogleMockRuns) {
    MockBatteryReader battery;
    EXPECT_CALL(battery, ReadPercent()).WillOnce(testing::Return(42));

    EXPECT_EQ(battery.ReadPercent(), 42);
}
