// benchmarks/bench_tick_to_trade.cpp — end-to-end tick-to-trade benchmark (skeleton).
// TODO: ITCH-in -> FH local book -> STRAT decision -> RISK -> OUCH-out; p50/p99/p99.9.
#include <benchmark/benchmark.h>

static void BM_Placeholder(benchmark::State& state) {
    for (auto _ : state) { benchmark::DoNotOptimize(0); }
}
BENCHMARK(BM_Placeholder);
BENCHMARK_MAIN();
