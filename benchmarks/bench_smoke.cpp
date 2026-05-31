// benchmarks/bench_smoke.cpp
// Smoke benchmark proving the Google Benchmark harness works AND showing the correct
// anti-optimization pattern (DoNotOptimize / ClobberMemory). NOT a real system benchmark —
// the meaningful ones live in bench_spsc_ring / bench_matching / bench_tick_to_trade.

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <cstring>

// Copy one (x86) cache line; the result is kept "used" so the compiler can't elide it.
static void BM_Memcpy64(benchmark::State& state) {
    alignas(64) char src[64];
    alignas(64) char dst[64];
    std::memset(src, 1, sizeof(src));
    for (auto _ : state) {
        std::memcpy(dst, src, sizeof(dst));
        benchmark::DoNotOptimize(dst);
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(sizeof(dst)));
}
BENCHMARK(BM_Memcpy64);

// Relaxed atomic read-modify-write — a stand-in for the kind of op a ring cursor does.
static void BM_AtomicIncrementRelaxed(benchmark::State& state) {
    std::atomic<std::uint64_t> counter{0};
    for (auto _ : state) {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        benchmark::DoNotOptimize(counter);
    }
}
BENCHMARK(BM_AtomicIncrementRelaxed);
