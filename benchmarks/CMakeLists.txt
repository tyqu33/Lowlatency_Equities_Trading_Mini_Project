# Microbenchmarks (Google Benchmark). Built only when -DHFT_BUILD_BENCHMARKS=ON.
# Link benchmark::benchmark_main (provides main()) so the per-topic files don't each need
# BENCHMARK_MAIN() and can share a single executable.
add_executable(hft_benchmarks
    bench_smoke.cpp
    bench_spsc_ring.cpp
    bench_matching.cpp
    bench_tick_to_trade.cpp)
target_link_libraries(hft_benchmarks PRIVATE
    hft::common hft::ipc hft::protocol
    benchmark::benchmark_main)
hft_set_warnings(hft_benchmarks)
