// benchmarks/bench_matching.cpp — Google Benchmark for the matching engine (skeleton).
// TODO: per-order match cost, add/cancel cost, throughput (orders/sec).
// No BENCHMARK_MAIN() here — main() comes from benchmark::benchmark_main (linked in CMake).
#include <benchmark/benchmark.h>

static void BM_MatchingPlaceholder(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(0);
    }
}
BENCHMARK(BM_MatchingPlaceholder);
