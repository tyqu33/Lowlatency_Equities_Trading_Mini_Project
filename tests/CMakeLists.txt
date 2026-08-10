# Unit tests (GoogleTest + GoogleMock). Built only when -DHFT_BUILD_TESTS=ON.
# Linking GTest::gmock_main provides main() and initializes BOTH gtest and gmock,
# so plain TEST() and gmock EXPECT_CALL tests share one executable.
include(GoogleTest)
add_executable(hft_tests
    test_price.cpp
    test_bits.cpp
    test_spsc_ring.cpp
    test_order_book.cpp
    test_risk_checks.cpp
    test_gmock_example.cpp)
target_link_libraries(hft_tests PRIVATE
    hft::common hft::ipc hft::protocol
    GTest::gmock_main)
hft_set_warnings(hft_tests)
gtest_discover_tests(hft_tests)
