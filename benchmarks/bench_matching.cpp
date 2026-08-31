// benchmarks/bench_matching.cpp — microbenchmarks for the two order book implementations.
// No BENCHMARK_MAIN() here — main() comes from benchmark::benchmark_main (linked in CMake).
//
// ============================================================================
// TWO IMPLEMENTATIONS, ONE WORKLOAD
// ============================================================================
//
// Every benchmark below is a template, registered twice: once against OrderBook (a std::map of
// price levels) and once against ArrayOrderBook (a flat tick-indexed array plus an occupancy
// bitmap). Same fixture, same seed, same order flow, same batch size. THE ONLY VARIABLE IS THE
// PRICE -> LEVEL CONTAINER, which is what makes the delta attributable to anything at all.
//
// docs/BENCHMARK-orderbook-v1.md committed three predictions before v2 was written:
//
//   1. `cancel` should flatten, with the largest gain at the largest depth. The O(log n) descent
//      becomes an array index and the pointer chasing that overflows cache at depth 1000 goes with
//      it. "If it does not, the analysis above is wrong somewhere."
//   2. The resting path should barely move. Its cost is two mallocs, and swapping the price
//      container does not remove them.
//   3. The crossing path should stay roughly where it is — at 13.7ns it is already doing almost
//      nothing but arithmetic.
//
// Read the output against those three. A prediction that turns out wrong is worth more here than
// one that turns out right, because it is the one that teaches you something about the machine.
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
// Every price this workload generates is a whole number of cents inside ArrayOrderBook's window
// ($79.52-$120.47): the deepest fixture reaches $90.00-$110.00 and the crossing orders price at
// $110.00. That is checked rather than assumed — see the fixture guard below.
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
//   Run everything:   ./build/bin/hft_benchmarks --benchmark_filter='OrderBook'
//   One operation:    ./build/bin/hft_benchmarks --benchmark_filter='Cancel'
//   One implementation:
//                     ./build/bin/hft_benchmarks --benchmark_filter='ArrayOrderBook'
//
// Benchmark names carry the template argument, so a row reads
//
//   BM_Cancel<hft::matching::ArrayOrderBook>/1000
//                                            ^^^^ depth: levels per side
//
// and the v1/v2 pair for a given operation appears as two blocks of three depths.
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
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/array_order_book.hpp"
#include "hft/matching/order_book.hpp"
#include "hft/matching/pool_order_book.hpp"

namespace {

using hft::common::OrderRefNum;
using hft::common::OrderType;
using hft::common::Price;
using hft::common::Qty;
using hft::common::Side;
using hft::common::TimeInForce;
using hft::matching::ArrayOrderBook;
using hft::matching::Fill;
using hft::matching::OrderBook;
using hft::matching::OrderRequest;
using hft::matching::PoolOrderBook;

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
//
// Templated on the book type so both implementations run against a byte-identical fixture: the
// same seed drives the same refs, the same prices and the same lot sizes into each.
template <typename Book>
struct PopulatedBook {
    Book book;
    std::vector<OrderRefNum> resting;
    Qty ask_qty_total{};
    Workload flow;

    explicit PopulatedBook(int depth) { rebuild(depth); }

    void rebuild(int depth) {
        book = Book{};
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

        // A book that silently refused this flow would look spectacularly fast, and nothing in the
        // timing loop would notice: ArrayOrderBook answers an off-grid or out-of-window price by
        // returning 0 and resting nothing. Check the fixture actually holds what we think it does,
        // once per rebuild, outside the timed region.
        const std::size_t expected = static_cast<std::size_t>(depth) * kOrdersPerLevel * 2;
        if (book.order_count() != expected) {
            std::fprintf(stderr,
                         "benchmark fixture is wrong: book holds %zu orders, expected %zu. The "
                         "book is rejecting prices this workload assumes it accepts.\n",
                         book.order_count(), expected);
            std::abort();
        }

        std::mt19937 shuffle_rng(kSeed);
        std::shuffle(resting.begin(), resting.end(), shuffle_rng);
    }
};

}  // namespace

// ============================================================================
// The ruler itself
// ============================================================================

// Not a benchmark of either book — a measurement of the instrument. Reports the smallest non-zero
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

template <typename Book>
static void BM_SubmitResting(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook<Book> fixture(depth);
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
BENCHMARK_TEMPLATE(BM_SubmitResting, OrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_SubmitResting, ArrayOrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_SubmitResting, PoolOrderBook)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// submit() — the matching path
// ============================================================================

// Each operation is a 100-share buy at the far touch: it crosses, consumes from the front of the
// best ask level, and rests nothing. 100 shares is at or below the minimum lot, so one operation
// never eats more than one resting order — the unit stays "one crossing order" rather than an
// average over however many orders happened to sit at that price.
template <typename Book>
static void BM_SubmitCrossing(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook<Book> fixture(depth);
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
BENCHMARK_TEMPLATE(BM_SubmitCrossing, OrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_SubmitCrossing, ArrayOrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_SubmitCrossing, PoolOrderBook)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// cancel() — the operation the book spends most of its life doing
// ============================================================================

// Refs are cancelled in shuffled order, so this measures the hash lookup plus an unlink from the
// middle of a level, not a favourable front-of-list walk.
template <typename Book>
static void BM_Cancel(benchmark::State& state) {
    const int depth = static_cast<int>(state.range(0));
    PopulatedBook<Book> fixture(depth);
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
            benchmark::DoNotOptimize(
                fixture.book.cancel(fixture.resting[next + static_cast<std::size_t>(i)]));
        }
        const auto t1 = Clock::now();

        samples.push_back(ns_between(t0, t1) / kBatchSize);
        next += kBatchSize;
    }
    report(state, samples);
}
BENCHMARK_TEMPLATE(BM_Cancel, OrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_Cancel, ArrayOrderBook)->Arg(10)->Arg(100)->Arg(1000);
BENCHMARK_TEMPLATE(BM_Cancel, PoolOrderBook)->Arg(10)->Arg(100)->Arg(1000);
