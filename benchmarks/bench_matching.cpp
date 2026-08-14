// benchmarks/bench_matching.cpp — microbenchmarks for hft::matching::OrderBook.
// No BENCHMARK_MAIN() here — main() comes from benchmark::benchmark_main (linked in CMake).
//
// ============================================================================
// THE WORKLOAD — read this before quoting any number below
// ============================================================================
//
// Synthetic, seeded, and deliberately simple. It is not a replay of real market data; it is a
// shape chosen to be *defensible*, which is a lower bar than *authentic* and the right one here:
// we are comparing two implementations of the same structure under an identical load, so the
// delta and the shape of the curve carry the meaning, not the absolute nanoseconds.
//
// Two properties of the shape are load-bearing, because getting them wrong would change the
// conclusion rather than just the numbers:
//
//   1. PRICES CLUSTER AT THE TOUCH. 90% of orders land within ±5 ticks of the reference price,
//      10% within ±50. Real order flow is far denser near the inside quote than away from it.
//      A uniform spread over a wide range would flatter the tree (which does not care) and
//      punish a tick-indexed array (which would page in a large, cold span); an unrealistically
//      narrow range would do the reverse and make the array look impossibly good.
//
//   2. CANCELS OUTNUMBER FILLS. In US equities the order-to-trade ratio is commonly cited in the
//      tens — it varies enormously by venue and participant, so treat that as an order of
//      magnitude, not a figure. Either way, cancel is the operation the book spends most of its
//      life doing, which is why it gets its own benchmark and why its cost is the number worth
//      quoting.
//
// Everything else (round-lot sizes, a single symbol, one price grid) is simplification that does
// not move the comparison.
//
// ============================================================================
// WHY OPERATIONS ARE TIMED IN BATCHES, NOT INDIVIDUALLY
// ============================================================================
//
// The obvious way to get percentiles is to timestamp either side of every single operation. That
// does not work on this hardware, and the reason is worth understanding because the API actively
// misleads you about it.
//
//   std::chrono::steady_clock reports  period = 1/1'000'000'000  — i.e. "nanoseconds".
//   Measured, the smallest non-zero gap between two consecutive reads is ~41.67ns, and every
//   observed gap is a multiple of it: 41, 83, 125, 166, ...
//
// Apple Silicon's userspace timer runs off a 24 MHz timebase, so 1/24MHz = 41.67ns is the real
// granularity. The nanosecond-denominated type is a unit, not a promise of resolution.
//
// The operations being measured here cost roughly 90-300ns. Timing them one at a time with a
// 41.67ns ruler quantises every sample to two or three possible values — a ~47% error on the
// smallest of them. The percentiles that come out are an artefact of the clock, not a property of
// the book. (An earlier version of this file did exactly that and reported a p50 of "42ns" for
// three different operations, which is simply the ruler's smallest mark.)
//
// The fix is not a faster machine — the CPU is not the limit, and the same clock reads the same
// way on a faster one. (Windows' QueryPerformanceCounter is commonly coarser still, at 100ns; the
// x86 answer for true per-operation resolution is rdtsc, which is not portable.) The fix is to
// measure a batch: run kBatchSize operations between two clock reads and divide. At 90ns/op a
// 64-op batch spans ~5.8us, so the quantisation error falls from ~47% to under 1%.
//
// What that costs: each sample is now the MEAN cost across a run of 64 operations, so a single
// pathological operation is averaged away rather than showing up in the tail. These percentiles
// describe how much the *sustained* cost varies, not how bad one unlucky call can get. For
// comparing two implementations that is the right measurement; for hunting a rare stall it is not.
//
// ============================================================================
// READING THE OUTPUT
// ============================================================================
//
//   Run:  ./build/bin/hft_benchmarks --benchmark_filter='OrderBook'
//
//   Time / CPU        per BATCH of kBatchSize operations — divide by it for per-op cost
//   items_per_second  already per-operation
//   p50/p99/p99.9_ns  per-operation, averaged within each batch
//   max_ns            almost always the OS, not the book: a preempted thread or a page fault.
//                     Measuring on a laptop under load, values in the tens of microseconds and up
//                     are environmental. Real latency work happens on tuned bare metal (isolated
//                     cores, interrupts moved away, turbo and C-states off), which is why the
//                     honest claim from a run like this is a RELATIVE improvement under identical
//                     conditions, never an absolute production latency figure.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/order_book.hpp"

namespace {

using hft::common::OrderRefNum;
using hft::common::OrderType;
using hft::common::Price;
using hft::common::Qty;
using hft::common::Side;
using hft::common::TimeInForce;
using hft::matching::Fill;
using hft::matching::OrderBook;
using hft::matching::OrderRequest;

// ---- Workload parameters ---------------------------------------------------

constexpr std::int64_t kRefTicks = 1'000'000;  // $100.0000
constexpr std::int64_t kTickSize = 100;        // one cent, in 1/10000 USD
constexpr int kNarrowSpanTicks = 5;            // 90% of flow lands within this many ticks
constexpr int kWideSpanTicks = 50;             // the other 10% lands within this many
constexpr double kNarrowShare = 0.90;
constexpr Qty kMinLot = 100;
constexpr Qty kMaxLot = 1000;
constexpr unsigned kSeed = 20260814;  // fixed: a surprising result must be reproducible

constexpr int kOrdersPerLevel = 10;
constexpr int kBatchSize = 64;  // see "WHY OPERATIONS ARE TIMED IN BATCHES" above

// ---- Percentiles -----------------------------------------------------------

// `samples` must already be sorted. Nearest-rank; fine at the sample counts here.
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t idx =
        std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
    return sorted[idx];
}

// `samples` holds per-operation costs (each already divided by the batch size).
void report(benchmark::State& state, std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    state.counters["p50_ns"] = percentile(samples, 0.50);
    state.counters["p99_ns"] = percentile(samples, 0.99);
    state.counters["p99.9_ns"] = percentile(samples, 0.999);
    state.counters["max_ns"] = samples.empty() ? 0.0 : samples.back();
    state.SetItemsProcessed(state.iterations() * kBatchSize);
}

using Clock = std::chrono::steady_clock;

double ns_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::nano>(b - a).count();
}

// ---- Order flow ------------------------------------------------------------

class Workload {
public:
    explicit Workload(unsigned seed = kSeed) : rng_(seed) {}

    // A price clustered at or below `ceiling`, so the caller can generate flow that is guaranteed
    // not to cross. Same distribution shape as real flow, mirrored onto one side.
    std::int64_t price_ticks_at_or_below(std::int64_t ceiling) {
        const int span = share_(rng_) < kNarrowShare ? kNarrowSpanTicks : kWideSpanTicks;
        std::uniform_int_distribution<int> offset(0, span);
        return ceiling - static_cast<std::int64_t>(offset(rng_)) * kTickSize;
    }

    Qty lot() {
        std::uniform_int_distribution<Qty> d(kMinLot / 100, kMaxLot / 100);
        return d(rng_) * 100;  // round lots
    }

    OrderRefNum next_ref() { return ++ref_; }

private:
    std::mt19937 rng_;
    std::uniform_real_distribution<double> share_{0.0, 1.0};
    OrderRefNum ref_{0};
};

OrderRequest limit_order(OrderRefNum ref, Side side, std::int64_t price_ticks, Qty qty) {
    OrderRequest r{};
    r.ref = ref;
    r.side = side;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::Day;
    r.qty = qty;
    r.limit_price = Price::from_ticks(price_ticks);
    return r;
}

// ---- Book construction -----------------------------------------------------

// A book `depth` levels deep on each side, `kOrdersPerLevel` orders at each price, with the touch
// straddling the reference price. Records every resting ref (shuffled) so cancel benchmarks do not
// get to walk the book in insertion order, and the total ask quantity so the matching benchmark
// knows how much it can eat before a rebuild is due.
struct PopulatedBook {
    OrderBook book;
    std::vector<OrderRefNum> resting;
    Qty ask_qty_total{};
    Workload flow;

    explicit PopulatedBook(int depth) { rebuild(depth); }

    void rebuild(int depth) {
        book = OrderBook{};
        resting.clear();
        ask_qty_total = 0;
        std::vector<Fill> fills;

        for (int level = 1; level <= depth; ++level) {
            const std::int64_t bid = kRefTicks - static_cast<std::int64_t>(level) * kTickSize;
            const std::int64_t ask = kRefTicks + static_cast<std::int64_t>(level) * kTickSize;
            for (int i = 0; i < kOrdersPerLevel; ++i) {
                const OrderRefNum b = flow.next_ref();
                book.submit(limit_order(b, Side::Buy, bid, flow.lot()), fills);
                resting.push_back(b);

                const OrderRefNum a = flow.next_ref();
                const Qty q = flow.lot();
                book.submit(limit_order(a, Side::Sell, ask, q), fills);
                resting.push_back(a);
                ask_qty_total += q;
            }
        }

        std::mt19937 shuffle_rng(kSeed);
        std::shuffle(resting.begin(), resting.end(), shuffle_rng);
    }
};

}  // namespace

// ============================================================================
// The ruler itself
// ============================================================================

// Not a benchmark of the book — a measurement of the instrument. Reports the smallest non-zero
// gap two consecutive clock reads can produce, which is the granularity every other number here
// is quantised to before batching divides it down.
static void BM_OrderBook_ClockGranularity(benchmark::State& state) {
    double smallest_nonzero = 1e9;
    for (auto _ : state) {
        const auto t0 = Clock::now();
        const auto t1 = Clock::now();
        const double d = ns_between(t0, t1);
        if (d > 0.0 && d < smallest_nonzero) {
            smallest_nonzero = d;
        }
    }
    state.counters["granularity_ns"] = smallest_nonzero;
    state.counters["batched_error_pct"] =
        100.0 * smallest_nonzero / (static_cast<double>(kBatchSize) * 90.0);
}
BENCHMARK(BM_OrderBook_ClockGranularity);

// ============================================================================
// submit() — the pure insert path (no crossing, order rests)
// ============================================================================

static void BM_OrderBook_SubmitResting(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook fixture(depth);
    std::vector<Fill> fills;
    std::vector<double> samples;
    samples.reserve(1u << 14);

    std::size_t placed = 0;
    for (auto _ : state) {
        // Keep the book near its nominal size. Rebuilding between batches rather than inside one
        // keeps the rebuild out of every sample, not just out of the average.
        if (placed + kBatchSize > fixture.resting.size()) {
            state.PauseTiming();
            fixture.rebuild(depth);
            placed = 0;
            state.ResumeTiming();
        }

        const auto t0 = Clock::now();
        for (int i = 0; i < kBatchSize; ++i) {
            const OrderRequest req = limit_order(fixture.flow.next_ref(), Side::Buy,
                                                 fixture.flow.price_ticks_at_or_below(kRefTicks), 100);
            benchmark::DoNotOptimize(fixture.book.submit(req, fills));
        }
        const auto t1 = Clock::now();

        samples.push_back(ns_between(t0, t1) / kBatchSize);
        placed += kBatchSize;
    }
    report(state, samples);
}
BENCHMARK(BM_OrderBook_SubmitResting)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// submit() — the matching path
// ============================================================================

// Each operation is a 100-share buy at the far touch: it crosses, consumes from the front of the
// best ask level, and rests nothing. 100 shares is at or below the minimum lot, so one operation
// never eats more than one resting order — the unit stays "one crossing order" rather than an
// average over however many orders happened to sit at that price.
static void BM_OrderBook_SubmitCrossing(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook fixture(depth);
    std::vector<Fill> fills;
    std::vector<double> samples;
    samples.reserve(1u << 14);

    Qty eaten = 0;
    for (auto _ : state) {
        if (eaten + kBatchSize * 100 > fixture.ask_qty_total) {
            state.PauseTiming();
            fixture.rebuild(depth);
            eaten = 0;
            state.ResumeTiming();
        }

        const auto t0 = Clock::now();
        for (int i = 0; i < kBatchSize; ++i) {
            // Price the order at the top of the ask range so it always crosses, whatever the
            // current touch is.
            const std::int64_t px = kRefTicks + static_cast<std::int64_t>(depth) * kTickSize;
            const OrderRequest req = limit_order(fixture.flow.next_ref(), Side::Buy, px, 100);
            benchmark::DoNotOptimize(fixture.book.submit(req, fills));
        }
        const auto t1 = Clock::now();

        samples.push_back(ns_between(t0, t1) / kBatchSize);
        eaten += kBatchSize * 100;
    }
    report(state, samples);
}
BENCHMARK(BM_OrderBook_SubmitCrossing)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// cancel() — the operation the book spends most of its life doing
// ============================================================================

// Refs are cancelled in shuffled order, so this measures the hash lookup plus an unlink from the
// middle of a level, not a favourable front-of-list walk.
static void BM_OrderBook_Cancel(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook fixture(depth);
    std::vector<double> samples;
    samples.reserve(1u << 14);

    std::size_t next = 0;
    for (auto _ : state) {
        if (next + kBatchSize > fixture.resting.size()) {
            state.PauseTiming();
            fixture.rebuild(depth);
            next = 0;
            state.ResumeTiming();
        }

        const auto t0 = Clock::now();
        for (int i = 0; i < kBatchSize; ++i) {
            benchmark::DoNotOptimize(fixture.book.cancel(fixture.resting[next + i]));
        }
        const auto t1 = Clock::now();

        samples.push_back(ns_between(t0, t1) / kBatchSize);
        next += kBatchSize;
    }
    report(state, samples);
}
BENCHMARK(BM_OrderBook_Cancel)->Arg(10)->Arg(100)->Arg(1000);
