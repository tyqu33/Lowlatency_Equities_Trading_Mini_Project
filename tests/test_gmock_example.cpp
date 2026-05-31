// tests/test_gmock_example.cpp
// Demonstrates GoogleMock wiring (a framework smoke test, NOT business logic).
// Self-contained: a tiny interface + mock proving gmock builds, links, and runs under ctest.
// Replace with real mocks (e.g., a fake venue, a fake clock) once components exist.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace {

// A trivial interface to mock (placeholder for a future injected dependency).
class IGreeter {
public:
    virtual ~IGreeter() = default;
    virtual int greet(int times) = 0;
};

class MockGreeter : public IGreeter {
public:
    MOCK_METHOD(int, greet, (int times), (override));
};

}  // namespace

using ::testing::Return;

TEST(GMockExample, ExpectCallReturnsConfiguredValue) {
    MockGreeter greeter;
    EXPECT_CALL(greeter, greet(3)).Times(1).WillOnce(Return(42));
    EXPECT_EQ(greeter.greet(3), 42);
}
