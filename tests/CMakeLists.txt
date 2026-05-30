# Unit tests (GoogleTest). Built only when -DHFT_BUILD_TESTS=ON.
include(GoogleTest)
add_executable(hft_tests
    test_price.cpp
    test_spsc_ring.cpp
    test_order_book.cpp
    test_risk_checks.cpp)
target_link_libraries(hft_tests PRIVATE
    hft::common hft::ipc hft::protocol GTest::gtest_main)
hft_set_warnings(hft_tests)
gtest_discover_tests(hft_tests)
