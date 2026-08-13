# hft::matching — single-symbol price-time-priority limit order book.
# A library rather than app code so that tests and benchmarks can link it.
add_library(hft_matching STATIC
    src/order_book.cpp)
add_library(hft::matching ALIAS hft_matching)
target_include_directories(hft_matching PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hft_matching PUBLIC hft::common)
target_compile_features(hft_matching PUBLIC cxx_std_20)
hft_set_warnings(hft_matching)
