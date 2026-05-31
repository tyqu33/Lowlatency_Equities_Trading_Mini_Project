// benchmarks/bench_spsc_ring.cpp — Google Benchmark for the SPSC ring (skeleton).
// TODO: enqueue/dequeue throughput + producer/consumer round-trip latency.
// No BENCHMARK_MAIN() here — main() comes from benchmark::benchmark_main (linked in CMake),
// so multiple benchmark files can share a single executable.
#include <benchmark/benchmark.h>

static void BM_SpscRingPlaceholder(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(0);
    }
}
BENCHMARK(BM_SpscRingPlaceholder);
