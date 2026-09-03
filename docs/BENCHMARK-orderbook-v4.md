# OrderBook v4 — no allocation left, and a measurement that had to be repaired

`hft::matching::FlatOrderBook` keeps everything v3 arrived at and replaces the last heap container:
the `std::unordered_map` reference index becomes an open-addressed hash table in a fixed array. With
that gone, **no operation on either hot path allocates or frees**.

Three rounds of measurement pointed at this structure, the third by failing to move. v3 pooled the
order nodes, dropped cancel's whole curve by ~30 ns, and left its slope unchanged to within 0.01 ns.
A cost that does not respond to removing one scattered structure belongs to the other one.

Raw output: [`benchmark-raw/orderbook-v1-v2.1-v3-v4-2026-09-02.txt`](benchmark-raw/orderbook-v1-v2.1-v3-v4-2026-09-02.txt).

```
cmake -S . -B build -DHFT_BUILD_BENCHMARKS=ON && cmake --build build -j
./build/bin/hft_benchmarks --benchmark_filter='OrderBook' --benchmark_min_time=0.5s
```

---

## The measurement had to be fixed before the results could be reported

This round produced a result about the harness before it produced a result about the book, and the
harness one is the more useful of the two.

**The batch size had been outgrown.** v1 established that this platform's clock ticks every 41.67 ns,
which makes timing a single ~90 ns operation meaningless, and fixed it by timing 64 operations
between two clock reads and dividing. At 90 ns each, a batch spans ~5.8 µs and one tick is 0.7% of
it. At v4's ~7 ns each, the same batch spans ~450 ns and one tick is **9%** of it.

The evidence was sitting in three consecutive runs of the same benchmark:

```
p50 = 7.17188      x 128 batch  ->  459 ns
p50 = 7.81250      x 128 batch  ->  500 ns
p50 = 7.81250      x 128 batch  ->  500 ns
                                    ^^^ 41 ns apart: exactly one clock tick
```

The reported percentile could only take values one tick apart. The optimisation had made the thing
being measured 18x faster than the instrument was set up for.

**What changed.** The batch is now 128 rather than 64 — the ceiling, because the shallowest fixture
holds only 200 resting orders and a batch may not exceed that. And the harness stopped reporting a
single quantisation figure: it had been hardcoded to assume 90 ns per operation and was still
printing 0.71% while the true figure for the fastest row was 10%. Every benchmark now computes its
own from its own measured p50, and that number appears in the tables below next to the value it
qualifies.

**What follows from it.** For the three slowest implementations nothing changes; their error stays
under 1%. For v4 the percentiles carry a real uncertainty, so this document quotes them to two
significant figures and leans on throughput, which is measured differently — Google Benchmark
accumulates over the whole iteration loop rather than per batch, so quantisation averages out over
millions of operations rather than landing on each sample.

**And a naming point that matters more than it looks.** A batch mean is a *throughput* figure: 128
independent operations, timed together, divided. It is not the latency of one operation from input
to output. Both are legitimate measurements and real order flow is mostly independent operations, so
throughput is the useful one here — but "127 M cancels/sec sustained" is a claim this harness
supports and "7 ns cancel latency" is not.

---

## Method

Otherwise unchanged: same seeded workload (90% of prices within ±5 ticks of $100.0000, 10% within
±50), same book shape, same machine, all four books in one process against a byte-identical fixture,
and the fixture still asserts it was populated before any timing runs.

**Environment.** MacBook Air (Apple Silicon, 8 cores, 64 KiB L1d, 4 MiB L2), macOS, ordinary desktop
load. Not tuned bare metal. **The honest claim from a run like this is a relative improvement under
identical conditions, never an absolute production latency figure.**

---

## Results

Per-operation nanoseconds, batch-averaged over 128. The v4 column carries its quantisation error,
because at these speeds it is no longer negligible.

### `cancel`

| metric | depth | v1 (`std::map`) | v2.1 (array) | v3 (pool) | v4 (flat) |
|---|---:|---:|---:|---:|---|
| p50 | 10 | 63.2 | 58.9 | 31.9 | **3.3** ±10% |
| | 100 | 95.4 | 69.7 | 40.4 | **4.6** ±7% |
| | 1000 | 134.1 | 82.7 | 52.7 | **7.5** ±4% |
| p99.9 | 1000 | 1087 | 931 | 155 | **15** |
| throughput | 1000 | 7.00 M/s | 11.35 M/s | 18.91 M/s | **127.8 M/s** |

Depth 10 → 1000, p50 growth: v1 **+70.9 ns**, v2.1 **+23.8 ns**, v3 **+20.8 ns**, v4 **+4.2 ns**.

### `submit` — the resting path (no cross)

| metric | depth | v1 | v2.1 | v3 | v4 |
|---|---:|---:|---:|---:|---|
| p50 | 10 | 75.2 | 57.9 | 42.3 | **24.7** ±1.3% |
| | 100 | 72.6 | 57.6 | 40.7 | **22.5** ±1.4% |
| | 1000 | 73.2 | 58.3 | 39.7 | **21.8** ±1.5% |
| throughput | 1000 | 13.04 M/s | 16.33 M/s | 28.47 M/s | **45.8 M/s** |

### `submit` — the crossing path (fully filled, rests nothing)

| metric | depth | v1 | v2.1 | v3 | v4 |
|---|---:|---:|---:|---:|---|
| p50 | 1000 | 14.6 | 15.3 | 11.4 | **7.5** ±4% |
| throughput | 1000 | 67.1 M/s | 64.6 M/s | 84.8 M/s | **131.5 M/s** |

### Net position against v1, depth 1000

| operation | v1 | v4 | |
|---|---:|---:|---|
| `cancel` | 7.00 M/s | **127.8 M/s** | **18.3x** |
| `submit` (rests) | 13.04 M/s | **45.8 M/s** | 3.5x |
| `submit` (crosses) | 67.1 M/s | **131.5 M/s** | 2.0x |

---

## Predictions and outcomes

The v3 write-up committed four predictions before v4 was written. Three were testable and all three
held — the first time in this project that the model made correct quantitative predictions rather
than being corrected by the result.

| # | prediction | outcome |
|---|---|---|
| 1 | The resting path loses another ~17 ns; if it loses ~5 ns the arithmetic is wrong | **Held.** 39.7 → 21.8 = **−17.9 ns** |
| 2 | `cancel`'s depth curve finally flattens; if the slope survives a third time the explanation is wrong | **Held.** Slope +20.8 → **+4.2 ns** |
| 3 | The p99.9 tail collapses further | **Held.** 155 → **15 ns** |
| 4 | v2's bitmap-scan hypothesis becomes testable | **Now testable, not yet tested.** Needs a separate experiment |
| 5 | Tombstone cost is invisible to this harness | **Confirmed invisible** — which is not the same as confirmed absent |

Running total across the project: **eleven predictions, three wrong, one half right.**

---

## Analysis

### 1. Prediction 2 was the one worth making, and it settled a two-round argument

The +19.5 ns slope in cancel survived v2 and v3 unchanged. Two implementations had been built on the
theory that it was the cost of chasing scattered memory, and neither had moved it. If v4 had left it
standing a third time, the explanation would have been wrong and the cause would have been somewhere
nobody had looked.

It went to +4.2 ns.

That is the whole argument closed: the growth was the `unordered_map` node chase, the nodes were
scattered because `malloc` placed them independently, and a flat array removes both the scattering
and the indirection.

### 2. Cancel gained 45 ns; the resting path gained 18. The difference is what each one asked the map to do

The obvious reading — "one allocation removed, so both paths should gain about the same ~17 ns" —
is wrong, and the way it is wrong is the interesting part.

| path | what it did with `unordered_map` |
|---|---|
| resting | **insert**: one allocation |
| cancel | **find**: chase a node `malloc` placed somewhere in ~1.5 MB, reached in shuffled order **and erase**: free that node |

The resting path was paying for one thing. Cancel was paying for three: an allocator call, a
cache-missing pointer chase, and the fact that the chase got worse with depth.

Two properties of the workload decide this, and only one of them is about the data structure:

- **Resting refs arrive in order.** The venue issues them from a counter, so consecutive orders
  touch consecutive buckets. Locality was already good and there was little to win.
- **Cancels arrive shuffled.** The benchmark shuffles deliberately, because real cancels do not
  arrive in submission order. That choice, made three rounds ago for realism, is the reason cancel
  is the only path that ever exposed the scattering.

There is also a size effect that has nothing to do with access order. A chained node costs roughly
72 bytes once the bucket pointer and the node header are counted; a `RefEntry` is 16. At 20 000 live
orders the lookup structure shrinks from ~1.5 MB to **320 KB**, and a cache line that used to carry
one useful entry now carries four.

### 3. What 7 ns actually is, including one hypothesis that was tested and did not survive

7.5 ns is about 24 cycles. The operation does a probe into a 320 KB table and then a dependent access
into a 640 KB pool — two accesses that cannot overlap with each other, since the second needs the
slot number from the first. An L2 round trip alone is several nanoseconds, so 24 cycles looked too
fast and was worth checking rather than reporting.

Three things account for it, and the first one proposed was the weakest.

**Instruction-level parallelism across operations — measured, worth ~2 ns.** Consecutive cancels are
independent of each other, so an out-of-order core can have several in flight even though the two
accesses *within* one cancel are serialised. Chaining consecutive cancels into a dependency chain,
so the next reference number cannot be computed until the previous cancel returns, cost 20.1 ns →
22.0 ns in a standalone harness. Real, but small. It had been offered as the main explanation and it
is not.

**The working set fits in L2 — this is most of it.** At depth 1000 the live ref table spans
20 000 × 16 B = 320 KB and the live pool 20 000 × 32 B = 640 KB. Both sit inside a 4 MiB L2. These
are L2 hits at a few nanoseconds, not the much more expensive misses the estimate assumed. The
allocator-scattered version had the same number of live orders but spread them over ~1.5 MB of nodes
plus bucket array, which is a different cache story with the same asymptotics.

**And some of it is the ruler.** At depth 10, cancel reads 3.3 ns with a 10% quantisation error. The
fastest numbers in this document are at the edge of what this clock can express, which is why they
are quoted to two figures and why the throughput column exists.

### 4. An independent reimplementation reproduces the ratio but not the absolute value

Before reporting an 18x improvement, the result was checked against a separately written harness
that builds the same fixture and times the same operations without Google Benchmark:

| | independent harness | this benchmark | ratio |
|---|---:|---:|---:|
| v3 `cancel` @1000 | 65.8 ns | 52.7 ns | 1.25 |
| v4 `cancel` @1000 | 9.1 ns | 7.5 ns | 1.27 |
| **v3 / v4** | **7.2x** | **7.4x** | |

The absolute numbers differ by ~25% between harnesses; the ratio agrees to within 3%. That is the
concrete form of a rule this project has stated since v1 — a run like this supports relative claims
under identical conditions and does not support absolute latency figures — and it is worth having
actually tested it rather than only asserting it.

A separate check confirmed the fast path was doing real work rather than failing early: 10 000
orders submitted, 10 000 cancels returning non-zero, book empty at the end, identical to v3.

---

## What this leaves

**Net position.** Four implementations, one 28-case typed suite, 140 tests green, and `book_demo`
produces byte-identical output from v2.1, v3 and v4 on the same script. Against v1 at depth 1000,
v4 sustains 18.3x the cancel throughput, 3.5x the resting throughput, and 2.0x the crossing
throughput, with no path slower than v1.

**Known limitations:**

- **Tombstones are unmeasured.** They accumulate with session length, not with book depth, and the
  harness rebuilds its fixture often enough to reset the table. This is a hole in the measurement,
  not a result, and it was written down in the header before the numbers came in. Measuring it needs
  a long-running single-book workload the harness does not currently have.
- **The fastest percentiles are quantisation-limited**, per the note above. Batch 128 is the ceiling
  the shallowest fixture allows.
- **Three fixed capacities**: 32 768 pooled orders, a 65 536-entry ref table, and the
  $79.52–$120.47 price window. Each rejects rather than growing.
- **`submit` returning 0 now means four different things** — filled completely, price refused, pool
  exhausted, ref table full. This should have been fixed two versions ago.
- **Memory is 2.2 MiB per book**, up from 1.1 MiB in v3 and a few hundred KB in v1. Reserving it up
  front is what "never allocates" costs.
- **Single symbol, single thread, no risk checks, no feed publication.** This is a book, not a
  matching engine.

**Next, if this line of work continues.** The four remaining questions are: whether tombstones cost
anything over a long session (needs a new workload shape); whether v2's out-of-order-execution
hypothesis survives now that the resting path is allocation-free (prediction 4, still untested); what
the crossing path's remaining 7.5 ns is made of; and whether any of it matters more than building the
components this book was meant to sit inside.
