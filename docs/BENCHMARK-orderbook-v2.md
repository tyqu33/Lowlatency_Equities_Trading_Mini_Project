# OrderBook v2 (tick-indexed array + occupancy bitmap) — measured against v1

`hft::matching::ArrayOrderBook` replaces v1's `std::map<Price, Level>` with a flat array of 4096
price levels indexed by `(price - base) / tick_size`, plus a 4096-bit occupancy bitmap per side
(64 `uint64_t` words) for locating the touch. Everything else is held fixed: the FIFO queue inside
each level is still `std::list`, the ref index is still `std::unordered_map`, and the matching logic
is the same walk outward from the touch. **One variable changes**, so the delta is attributable.

[`BENCHMARK-orderbook-v1.md`](BENCHMARK-orderbook-v1.md) committed three predictions before v2 was
written. One held, one held in shape but not in size, and **one was wrong in the opposite
direction** — that last one is the most useful result in this document.

Source: [`benchmarks/bench_matching.cpp`](../benchmarks/bench_matching.cpp). Reproduce with

```
cmake -S . -B build -DHFT_BUILD_BENCHMARKS=ON && cmake --build build -j
./build/bin/hft_benchmarks --benchmark_filter='OrderBook' --benchmark_min_time=0.5s
```

---

## Method

Unchanged from v1 in every respect that could move a number: same synthetic seeded workload (90% of
prices within ±5 ticks of $100.0000, 10% within ±50), same book shape (`depth` levels per side, 10
orders per level, round lots 100–1000), same batch-of-64 timing, same machine. See the v1 document
for why the workload has that shape and why operations are timed in batches rather than
individually.

Three things are new:

**Both books run the same fixture.** The benchmarks are templates registered twice, so the same seed
drives the same refs, prices and lot sizes into each implementation. Neither book gets a workload
tuned for it.

**v1's column is a fresh re-run, not a copy.** The v1 numbers below were measured in the same
process as v2's, minutes apart, under the same desktop load. Comparing v2 against numbers recorded
on a different day would fold ambient variation into the result. (They land close to the v1
document's figures, which is reassuring but not something to rely on.)

**The fixture asserts it was actually populated.** `ArrayOrderBook` answers a price that is off its
grid or outside its window by returning 0 and resting nothing. A book that silently refused the
whole workload would look spectacularly fast and nothing in the timing loop would notice, so the
fixture checks `order_count()` against the expected count after every rebuild and aborts if they
disagree. Every number below was taken with that check passing.

Clock granularity measured at 41 ns; batching 64 operations brings the quantisation error to
**0.71%**.

**Environment.** MacBook Air (Apple Silicon, 8 cores, 64 KiB L1d, 4 MiB L2), macOS, ordinary desktop
load. Not tuned bare metal. **The honest claim from a run like this is a relative improvement under
identical conditions, never an absolute production latency figure.**

---

## Results

Per-operation nanoseconds, batch-averaged. Lower is better; **bold** marks the winner.

| Operation | metric | depth | v1 (`std::map`) | v2 (array + bitmap) | Δ |
|---|---|---:|---:|---:|---:|
| **cancel** | p50 | 10 | 67.0 | **62.5** | −6.8% |
| | | 100 | 95.1 | **69.7** | −26.7% |
| | | 1000 | 133.5 | **82.7** | **−38.1%** |
| | p99 | 10 | 80.1 | **76.8** | −4.1% |
| | | 100 | 113.9 | **81.4** | −28.6% |
| | | 1000 | 229.8 | **135.4** | −41.1% |
| | ops/s | 1000 | 7.0 M | **11.3 M** | +61% |
| **submit** (rests) | p50 | 10 | 74.9 | **61.2** | −18.2% |
| | | 100 | 72.3 | **60.5** | −16.2% |
| | | 1000 | 71.6 | **59.3** | −17.3% |
| | ops/s | 1000 | 13.0 M | **15.6 M** | +20% |
| **submit** (crosses) | p50 | 10 | **14.3** | 25.4 | **+77%** |
| | | 100 | **14.3** | 24.8 | +73% |
| | | 1000 | **14.3** | 27.3 | +91% |
| | ops/s | 1000 | **66.6 M** | 35.8 M | −46% |

Depth scaling of `cancel` p50, which is the headline:

| | depth 10 → 1000 |
|---|---:|
| v1 | 67.0 → 133.5 ns (**+99%**) |
| v2 | 62.5 → 82.7 ns (**+32%**) |

---

## Verdict on the three predictions

| # | Prediction | Outcome |
|---|---|---|
| 1 | `cancel` should flatten, largest gain at largest depth | **Held.** −38% at depth 1000, −6.8% at depth 10 — the gain scales with depth exactly as claimed. |
| 2 | The resting path should barely move | **Half right.** The *shape* held (flat across depth, both books). The *size* did not: 17% is more than "barely". |
| 3 | The crossing path should stay roughly where it is | **Wrong, and in the opposite direction.** v2 is ~1.8× slower. |

---

## Analysis

### 1. `cancel` improved as predicted — and what remains is not a logarithm

The v1 analysis attributed cancel's depth curve to one line: the `O(log n)` red-black tree descent
in `buy.find(price)`. Deleting that line deletes most of the curve. v1 doubles between depth 10 and
1000; v2 grows by a third.

But v2 does not go flat, and it is worth being precise about why, because the obvious answer is
wrong. The remaining work in `cancel` is:

| step | complexity |
|---|---|
| `refMap.find(ref)` | **O(1) average** — `unordered_map` is a hash table, not a tree |
| index into `buy_levels_[idx]` | O(1) |
| `orders.erase(it)` | O(1), iterator already in hand |
| clear the occupancy bit | O(1) |

Every step is O(1). The 20 ns of growth from depth 10 to depth 1000 is therefore **not** a
complexity effect — it is a working-set effect:

| depth | resting orders | `refMap` nodes + buckets | touched `Level`s |
|---:|---:|---|---|
| 10 | 200 | ~15 KB | 20 of 4096 |
| 100 | 2 000 | ~150 KB | 200 of 4096 |
| 1000 | 20 000 | ~1.5 MB | 2000 of 4096 |

At depth 1000 the hash node for a given ref, and the `std::list` node it points at, are two
independently `malloc`'d objects somewhere in a 1.5 MB working set, reached by cancels arriving in
shuffled order. The number of pointer hops is constant; the cost of each hop is not.

> **O(1) says how many times you jump, not how far.** Complexity analysis stops at the first;
> latency work lives in the second. Flattening the rest of this curve means attacking allocation —
> an intrusive list over a pre-allocated pool, and a ref index that is not a chain of heap nodes —
> which is a different change from the one measured here.

### 2. The resting path: the shape confirmed the model, the size corrected it

Both implementations are flat across a hundredfold change in depth (v1: 74.9 / 72.3 / 71.6; v2:
61.2 / 60.5 / 59.3). That flatness is the load-bearing observation. If the price-container lookup
dominated this path, v1 would curve upward with depth the way `cancel` does. It does not, so the
v1 finding stands: **the resting path's cost is dominated by its two heap allocations**, one
`std::list` node and one `unordered_map` node, and neither is affected by swapping the price
container.

What the prediction got wrong is the size of the residue. Replacing a tree descent with
`(price - base) / tick_size` is worth a flat **~12 ns** — visible, consistent across all three
depths, and about 17% of a ~72 ns operation. "Barely move" was too strong.

A detail worth keeping, because it explains why v1's own two `std::map` operations behave so
differently. The resting path and `cancel` both do one map lookup, yet only `cancel` curves:

- resting prices cluster within ±50 ticks of the reference, so `buy[bid_price]` lands in the same
  small, hot region of the tree no matter how deep the book is;
- cancels arrive in **shuffled** order and reach across the entire book.

Same data structure, same complexity, opposite curves — decided entirely by locality. This is the
clearest evidence in either document that at these time scales, cache behaviour explains more than
asymptotics does.

### 3. The crossing path got 1.8× slower, and the cause is a design gap, not the array

This is the result worth dwelling on. v2 lost, badly, on the one path that was predicted not to
change.

The cause is not the array. It is that **v2 does not remember where the touch is, and v1 does.**

| | getting the best opposite level |
|---|---|
| v1 | `sell.begin()` — libc++'s `__tree` caches its leftmost node, so this is ~1 dereference |
| v2 | `next_up(sell_occupied_, 0)` — scan the bitmap from word 0 until a non-zero word appears |

The fixture centres the book on $100.0000, which is index 2048, so the shallowest ask sits at index
2049 — **word 32**. Every crossing order therefore scans words 0 through 31 and finds nothing before
reaching the first occupied word.

The arithmetic closes:

```
32 iterations of a tight, dependency-free loop over 256 bytes (4 cache lines, all L1-resident)
≈ 32 cycles ≈ 10 ns at ~3.2 GHz

measured v1 → v2 delta:  14.3 → 25.4 ns  =  +11.1 ns
```

Two further observations confirm it rather than merely fitting it:

- **v2's crossing cost is flat across depth** (25.4 / 24.8 / 27.3 ns). The distance from index 0 to
  the touch does not depend on how deep the book is — only on where the book sits in the window.
- Which means this benchmark, for v2, is partly **measuring how far the reference price happens to
  be from the left edge of the price window**. A fixture centred near $79.52 would make the
  regression nearly vanish; one near $120.47 would make it worse. A number that moves when you
  relabel your axes is a number describing the design, not the workload.

The fix is the thing every real venue does and v2 omits: **cache the touch.** Maintain the best bid
and best ask index incrementally — widen on a resting order that improves the touch, narrow when a
level is emptied by a fill or a cancel — and start both `best_*()` and the matching cursor from that
hint instead of from the end of the array. `std::map` gets this for free because the tree caches its
leftmost node; an array has to be told.

That is deferred to v2.1 rather than folded in here, for the same reason the rest of the book was
held fixed: a change measured on its own is attributable, and a regression this cleanly explained is
worth recording before it is erased.

---

## What this leaves

**Net position.** v2 wins decisively on the operation that matters most — `cancel` is 38% faster at
depth 1000 and, more importantly, degrades a third as fast with depth. It wins modestly on the
resting path. It loses on the crossing path for a reason that is understood, quantified, and
fixable.

**Known limitations, stated rather than hidden:**

- **The price window is fixed** at $79.52–$120.47 on a one-cent grid. Prices outside it, or off the
  grid, are rejected. A real venue rebases the window as the market moves; this version does not.
- **`submit` returning 0 is ambiguous** — it means "filled completely" *or* "refused". Worth fixing
  before the book is wired to anything that has to answer a participant.
- **The touch is not cached**, per finding (3).
- **Allocation is untouched.** Both books `malloc` twice per resting order. Finding (1) says this is
  now the dominant remaining term in `cancel`'s depth curve, and finding (2) says it dominates the
  resting path outright.

**Next measurement.** v2.1 with a cached touch, run three-way against v1 and v2. Prediction, stated
before the change: the crossing path returns to roughly v1's 14 ns and the two `best_*()` accessors
become O(1); `cancel` and the resting path move very little, because neither of them scans for the
touch.
