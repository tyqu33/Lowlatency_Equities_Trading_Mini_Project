// benchmarks/bench_spsc_ring.cpp — Google Benchmark for the SPSC ring (skeleton).
// TODO: enqueue/dequeue throughput + round-trip latency (HdrHistogram).
#include <benchmark/benchmark.h>

static void BM_Placeholder(benchmark::State& state) {
    for (auto _ : state) { benchmark::DoNotOptimize(0); }
}
BENCHMARK(BM_Placeholder);
BENCHMARK_MAIN();
