# OrderBook v1 (`std::map`) — baseline measurements

Baseline for `hft::matching::OrderBook`, taken before replacing the `std::map` price levels with a
tick-indexed array. The numbers exist to be compared against that replacement; the analysis below
is what makes the comparison mean something.

Source: [`benchmarks/bench_matching.cpp`](../benchmarks/bench_matching.cpp). Reproduce with

```
cmake -S . -B build -DHFT_BUILD_BENCHMARKS=ON && cmake --build build -j
./build/bin/hft_benchmarks --benchmark_filter='OrderBook' --benchmark_min_time=0.3s
```

---

## Method

**Workload.** Synthetic and seeded, not a market-data replay. Two properties of its shape are
load-bearing, in that getting them wrong would change the conclusion rather than just the numbers:

- **Prices cluster at the touch** — 90% of orders land within ±5 ticks of the reference price, 10%
  within ±50. Spreading orders uniformly over a wide range would flatter a tree (indifferent to
  key locality) and punish a tick-indexed array (which would page in a large cold span). An
  unrealistically narrow range does the reverse.
- **Cancels dominate.** Cancel gets its own benchmark and its own depth sweep because it is the
  operation a real book spends most of its life performing.

**Book state.** `depth` price levels per side, 10 orders per level, straddling $100.0000 on a
one-cent grid. Round lots between 100 and 1000 shares. Rebuilt between batches, never inside one,
so setup never lands in a sample.

**Timing.** Operations are timed in batches of 64 rather than individually. This is not a
preference — it is forced by the clock:

```
std::chrono::steady_clock reports period = 1/1'000'000'000   ("nanoseconds")
Smallest observable non-zero gap, measured:   ~41.67 ns
Every observed gap:                           a multiple of it — 41, 83, 125, 166, …
```

Apple Silicon's userspace timer runs off a 24 MHz timebase, so 41.67 ns is the real granularity;
the nanosecond-denominated type is a unit, not a resolution. Timing a ~70 ns operation with a
41.67 ns ruler quantises every sample to two or three possible values. Batching 64 operations
between two clock reads drops the quantisation error from ~47% to **0.71%** (reported by
`BM_OrderBook_ClockGranularity`).

The cost of batching: each sample is the *mean* over 64 consecutive operations, so a single
pathological call is averaged away instead of appearing in the tail. These percentiles describe how
much the sustained cost varies, not how bad one unlucky call can be.

**Environment.** MacBook Air (Apple Silicon, 8 cores, 64 KiB L1d, 4 MiB L2), macOS, ordinary desktop
load. Not tuned bare metal — no core isolation, no interrupt steering, turbo and C-states active.
**The honest claim from a run like this is a relative improvement under identical conditions, never
an absolute production latency figure.**

---

## Results

Per-operation nanoseconds, batch-averaged.

| Operation | metric | depth 10 | depth 100 | depth 1000 |
|---|---|---:|---:|---:|
| **submit** (rests, no cross) | p50 | 74.9 | 72.9 | 71.0 |
| | p99 | 89.8 | 184.3 | 102.9 |
| | p99.9 | 173.8 | 203.1 | 897.1 |
| | ops/s | 12.8 M | 13.0 M | 13.1 M |
| **submit** (crosses, fully filled) | p50 | 13.7 | 13.7 | 14.3 |
| | p99 | 18.9 | 18.9 | 19.5 |
| | p99.9 | 21.5 | 21.5 | 23.4 |
| | ops/s | 65.2 M | 68.9 M | 67.3 M |
| **cancel** | p50 | **65.8** | **95.0** | **135.4** |
| | p99 | 78.8 | 114.6 | 259.1 |
| | p99.9 | 166.7 | 207.0 | 2020.8 |
| | ops/s | 14.3 M | 10.4 M | 6.9 M |

---

## Analysis

### 1. Only `cancel` scales with depth, and the growth is the red-black tree

Cancel's p50 goes 65.8 → 95.0 → 135.4 ns as depth goes 10 → 100 → 1000. A hundredfold increase in
levels costs roughly a doubling in time, which is the signature of a logarithm. Fitting
`cost = a + b·log₂(depth)` through the two endpoints gives `b ≈ 10.5`, `a ≈ 30.9`, and predicts
**100.6 ns** at depth 100 against **95.0 ns** measured — a 6% fit.

The growth comes from exactly one line. `cancel` performs three lookups:

| lookup | complexity | contributes to the curve? |
|---|---|---|
| `refMap.find(ref)` | O(1) hash | no |
| `buy.find(price)` / `sell.find(price)` | **O(log n)** red-black tree | **yes** |
| `orders.erase(it)` | O(1) — the iterator is already in hand | no |

Unlinking from the intrusive position is free; finding *which* level to unlink from is not. That
single tree descent is the whole story, and it is what the array replacement deletes.

### 2. A crossing order that fully fills is ~5× cheaper than one that rests

13.7 ns versus ~73 ns. The crossing path returns as soon as `remaining` hits zero, which skips the
entire resting block:

```cpp
Level& l = buy[bid_price];      // O(log n) descent, plus a node allocation if the level is new
l.orders.push_back(order);      // heap allocation — one std::list node
refMap[req.ref] = loc;          // heap allocation — one unordered_map node
```

The two heap allocations, not the tree descent, are the dominant term. That is also why the resting
path is **flat across depth** (74.9 / 72.9 / 71.0): going from a 3-level tree walk to a 10-level one
adds a few nanoseconds, which disappears inside ~60 ns of allocator work.

This matters for what v2 can be expected to achieve — see below.

### 3. The p99.9 tail is the operating system; the structural effect is in the p50

Cancel at depth 1000 shows p99.9 = 2020 ns against p50 = 135 ns, a 15× spread where depth 10 shows
only 2.5×. The tempting reading is that a red-black tree rebalance occasionally goes long. The
arithmetic rules that out.

Each sample is the mean of 64 operations, so a p99.9 sample of 2020 ns means that batch took
`2020 × 64 ≈ 129 µs`. If one call in the batch is responsible, that call took roughly **128 µs**. A
red-black tree deletion performs at most three rotations — tens of nanoseconds. **Three orders of
magnitude apart.** That tail is thread preemption or a page fault, and `max_ns = 51069` (51 µs) is
the same thing, less disguised.

The real structural effect hides in the p50, and it is cache, not rotations. Fitting the logarithm
through depth 10 and depth 100 alone and extrapolating predicts **124.3 ns** at depth 1000; the
measurement is **135.4 ns**, about 9% above the line. The excess has a plausible cause:

| depth | map nodes × ~75 B | vs 64 KiB L1d |
|---|---:|---|
| 10 | 0.75 KB | fits easily |
| 100 | 7.5 KB | fits |
| **1000** | **~75 KB** | **overflows** |

A tree descent at depth 1000 touches ~10 nodes scattered across a working set larger than L1, so
most of those hops miss. Below L1 the logarithm is nearly free; above it, every level of the descent
costs a trip to L2.

---

## What this predicts about v2

The replacement indexes price levels by tick directly, so:

- **`cancel` should flatten.** The O(log n) descent becomes an array index, and the pointer chasing
  that causes the cache overflow at depth 1000 goes with it. The largest improvement should appear
  at the largest depth — and if it does not, the analysis above is wrong somewhere.
- **The resting path should barely move** unless allocation is addressed too. Finding (2) says its
  cost is two `malloc`s, and swapping the price container does not remove them. Getting that number
  down needs an intrusive list over a pre-allocated pool, which is a separate change.
- **The crossing path should stay roughly where it is.** At 13.7 ns it is already doing almost
  nothing but arithmetic.

Stating these before the change is deliberate: a prediction that could be wrong is worth more than
an explanation produced after the fact.
