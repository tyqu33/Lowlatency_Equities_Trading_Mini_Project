# hft::common — fixed-point Price, symbol interning, types, timing, CPU affinity.
add_library(hft_common STATIC
    src/price.cpp
    src/symbol.cpp
    src/time.cpp
    src/cpu.cpp)
add_library(hft::common ALIAS hft_common)
target_include_directories(hft_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hft_common PUBLIC cxx_std_20)
hft_set_warnings(hft_common)
