# Microbenchmarks (Google Benchmark). Built only when -DHFT_BUILD_BENCHMARKS=ON.
add_executable(hft_benchmarks
    bench_spsc_ring.cpp
    bench_matching.cpp
    bench_tick_to_trade.cpp)
target_link_libraries(hft_benchmarks PRIVATE
    hft::common hft::ipc hft::protocol benchmark::benchmark)
