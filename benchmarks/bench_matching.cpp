// benchmarks/bench_matching.cpp — Google Benchmark for the matching engine (skeleton).
// TODO: per-order match cost, add/cancel cost, throughput (orders/sec).
#include <benchmark/benchmark.h>

static void BM_Placeholder(benchmark::State& state) {
    for (auto _ : state) { benchmark::DoNotOptimize(0); }
}
BENCHMARK(BM_Placeholder);
BENCHMARK_MAIN();
