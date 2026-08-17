# OrderBook v2 → v2.1 — tick-indexed array, occupancy bitmap, cached touch

Two changes to `hft::matching::OrderBook`, measured one at a time against the
[v1 baseline](BENCHMARK-orderbook-v1.md).

**v2** replaces v1's `std::map<Price, Level>` with a flat array of 4096 price levels indexed by
`(price - base) / tick_size`, plus a 4096-bit occupancy bitmap per side (64 `uint64_t` words) for
locating the touch. Everything else is held fixed: the FIFO queue inside each level is still
`std::list`, the ref index is still `std::unordered_map`, and the matching logic is the same walk
outward from the touch.

**v2.1** adds one thing v2 lacked and `std::map` had for free: a **cached touch**. The best bid and
best ask indices are maintained incrementally, so neither the read accessors nor the matching cursor
have to search for them.

The order matters. v2's measurement produced a regression that v2.1 exists to fix, and the arc from
*predict → measure → diagnose → fix → re-measure* is the substance of this document. A single
"v2.1 is faster than v1" table would have been shorter and worth much less.

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
for why the workload has that shape and why operations are timed in batches rather than individually.

Three things are new:

**Both books run the same fixture.** The benchmarks are templates registered twice, so the same seed
drives the same refs, prices and lot sizes into each implementation. Neither book gets a workload
tuned for it.

**The fixture asserts it was actually populated.** `ArrayOrderBook` answers a price that is off its
grid or outside its window by returning 0 and resting nothing. A book that silently refused the whole
workload would look spectacularly fast and nothing in the timing loop would notice, so the fixture
checks `order_count()` against the expected count after every rebuild and aborts if they disagree.
Every number below was taken with that check passing.

**Two runs, disclosed.** v2.1 replaced v2 in place, so the v2 column comes from a separate run a few
minutes earlier. v1 was measured in *both* runs and moved by at most 1.3 ns across all nine
measurements — which is the evidence that the two runs are comparable, rather than an assumption
that they are.

Clock granularity measured at 41 ns; batching 64 operations brings the quantisation error to **0.71%**.

**Environment.** MacBook Air (Apple Silicon, 8 cores, 64 KiB L1d, 4 MiB L2), macOS, ordinary desktop
load. Not tuned bare metal. **The honest claim from a run like this is a relative improvement under
identical conditions, never an absolute production latency figure.**

---

## Results

Per-operation nanoseconds, batch-averaged. **Bold** marks the best of the three.

### `cancel` — the operation the book spends most of its life doing

| metric | depth | v1 (`std::map`) | v2 (array+bitmap) | v2.1 (+cached touch) |
|---|---:|---:|---:|---:|
| p50 | 10 | 67.0 | **62.5** | 63.1 |
| | 100 | 95.1 | **69.7** | **69.7** |
| | 1000 | 133.5 | 82.7 | **82.0** |
| p99 | 1000 | 224.0 | 132.2 | **132.2** |
| ops/s | 1000 | 7.0 M | 11.4 M | **11.4 M** |

Depth scaling of p50, 10 → 1000: **v1 +99%**, **v2/v2.1 +30%**.

### `submit` — the resting path (no cross)

| metric | depth | v1 | v2 | v2.1 |
|---|---:|---:|---:|---:|
| p50 | 10 | 74.9 | 61.2 | **57.3** |
| | 100 | 72.3 | 60.5 | **57.3** |
| | 1000 | 72.9 | 59.3 | **56.6** |
| p99 | 1000 | 166.7 | 89.8 | **84.6** |
| ops/s | 1000 | 12.5 M | 15.6 M | **16.4 M** |

### `submit` — the crossing path (fully filled, rests nothing)

| metric | depth | v1 | v2 | v2.1 |
|---|---:|---:|---:|---:|
| p50 | 10 | **14.3** | 25.4 | 15.0 |
| | 100 | **14.3** | 24.8 | 15.0 |
| | 1000 | **14.3** | 27.3 | 15.0 |
| ops/s | 1000 | **66.7 M** | 35.8 M | 64.1 M |

### Net position, v2.1 against v1 at depth 1000

| operation | v1 | v2.1 | Δ |
|---|---:|---:|---:|
| `cancel` | 133.5 ns | **82.0 ns** | **−38.5%** |
| `submit` (rests) | 72.9 ns | **56.6 ns** | **−22.3%** |
| `submit` (crosses) | 14.3 ns | 15.0 ns | +4.6% (parity) |

---

## Predictions and outcomes

The v1 document committed three predictions before v2 was written; this document committed three
more before v2.1 was written. Stating them first is deliberate — an explanation produced after the
fact is worth much less, and is hard to distinguish from one that was fitted to the answer.

| stage | prediction | outcome |
|---|---|---|
| v2 | `cancel` flattens, largest gain at largest depth | **Held.** −38% at depth 1000, −6.8% at depth 10 |
| v2 | resting path barely moves | **Half.** Shape held (flat across depth); size did not — 17% |
| v2 | crossing path stays where it is | **Wrong, opposite direction.** ~1.8× slower |
| v2.1 | crossing path returns to ~14 ns | **Held.** 15.0 ns vs v1's 14.3 ns |
| v2.1 | `cancel` moves very little | **Held.** Within 0.6 ns at all three depths |
| v2.1 | resting path moves very little | **Half.** Moved another −3.9 ns (−6.4%) — see (4) |

---

## Analysis

### 1. `cancel` improved as predicted — and what remains is not a logarithm

The v1 analysis attributed cancel's depth curve to one line: the `O(log n)` red-black tree descent in
`buy.find(price)`. Deleting that line deletes most of the curve. v1 doubles between depth 10 and
1000; v2 grows by a third. v2.1 is unchanged from v2, exactly as predicted — `cancel` reaches its
level through the ref index and never searches for the touch, so caching the touch cannot help it.

v2 does not go flat, and the obvious explanation for the residue is wrong. The remaining work is:

| step | complexity |
|---|---|
| `refMap.find(ref)` | **O(1) average** — `unordered_map` is a hash table, not a tree |
| index into `buy_levels_[idx]` | O(1) |
| `orders.erase(it)` | O(1), iterator already in hand |
| clear the occupancy bit, maybe recompute the touch | O(1) / bounded scan, only when the touch level empties |

Every step is O(1). The 19 ns of growth from depth 10 to depth 1000 is therefore **not** a complexity
effect — it is a working-set effect:

| depth | resting orders | `refMap` nodes + buckets | touched `Level`s |
|---:|---:|---|---|
| 10 | 200 | ~15 KB | 20 of 4096 |
| 100 | 2 000 | ~150 KB | 200 of 4096 |
| 1000 | 20 000 | **~1.5 MB** | 2000 of 4096 |

At depth 1000 the hash node for a given ref, and the `std::list` node it points at, are two
independently `malloc`'d objects somewhere in a 1.5 MB working set, reached by cancels arriving in
shuffled order. The number of pointer hops is constant; the cost of each hop is not.

> **O(1) says how many times you jump, not how far.** Complexity analysis stops at the first;
> latency work lives in the second. Flattening the rest of this curve means attacking allocation —
> an intrusive list over a pre-allocated pool, and a ref index that is not a chain of heap nodes.

### 2. The resting path: the shape confirmed the model, the size corrected it

All three implementations are flat across a hundredfold change in depth (v1: 74.9 / 72.3 / 72.9;
v2.1: 57.3 / 57.3 / 56.6). That flatness is the load-bearing observation. If the price-container
lookup dominated this path, v1 would curve upward with depth the way `cancel` does. It does not, so
the v1 finding stands: **the resting path's cost is dominated by its two heap allocations**, one
`std::list` node and one `unordered_map` node, and neither is affected by swapping the price
container.

What the prediction got wrong is the size of the residue. Replacing `buy[bid_price]` — an `O(log n)`
tree descent that also allocates a map node when the level is new — with `(price - base) / tick_size`
is worth a flat **~13 ns**, consistent across all three depths. "Barely move" was too strong.

A detail worth keeping, because it explains why v1's own two `std::map` operations behave so
differently. The resting path and `cancel` both do one map lookup, yet only `cancel` curves:

- resting prices cluster within ±50 ticks of the reference, so `buy[bid_price]` lands in the same
  small, hot region of the tree no matter how deep the book is;
- cancels arrive in **shuffled** order and reach across the entire book.

Same data structure, same complexity, opposite curves — decided entirely by locality. This is the
clearest evidence in either document that at these time scales, cache behaviour explains more than
asymptotics does.

### 3. The crossing regression, and what caused it

This is the result worth dwelling on. v2 lost, badly, on the one path that was predicted not to
change: 14.3 → 25.4 ns, a 77–91% regression.

The cause is not the array. It is that **v2 did not remember where the touch was, and v1 did.**

| | getting the best opposite level |
|---|---|
| v1 | `sell.begin()` — libc++'s `__tree` caches its leftmost node, so this is ~1 dereference |
| v2 | `next_up(sell_occupied_, 0)` — scan the bitmap from word 0 until a non-zero word appears |

The fixture centres the book on $100.0000, which is index 2048, so the shallowest ask sits at index
2049 — **word 32**. Every crossing order therefore scanned words 0 through 31 and found nothing
before reaching the first occupied word.

The arithmetic closes:

```
32 iterations of a tight, dependency-free loop over 256 bytes (4 cache lines, all L1-resident)
≈ 32 cycles ≈ 10 ns at ~3.2 GHz

measured v1 → v2 delta:  14.3 → 25.4 ns  =  +11.1 ns
```

Two further observations confirm it rather than merely fitting it:

- **v2's crossing cost was flat across depth** (25.4 / 24.8 / 27.3 ns). The distance from index 0 to
  the touch does not depend on how deep the book is — only on where the book sits in the window.
- Which means that benchmark, for v2, was partly **measuring how far the reference price happened to
  be from the left edge of the price window**. A fixture centred near $79.52 would have made the
  regression nearly vanish; one near $120.47 would have made it worse. A number that moves when you
  relabel your axes is describing the design, not the workload.

It is worth being explicit about what this says of the two structures. `std::map` is not beaten
everywhere by an array: an ordered tree hands you `begin()` for free, and an array has to be told.
v2 traded a fast lookup-by-price for a slow find-the-minimum without noticing it had done so.

**v2.1 keeps two indices** — `best_bid_idx_` and `best_ask_idx_`, with `kLevels` as the "no orders"
sentinel — maintained by two small helpers that own every bitmap mutation in the class:

```
on_level_occupied(side, idx)   set the bit; widen the touch if this level is better
on_level_emptied(side, idx)    clear the bit; if this level WAS the touch, find the next one
```

The bitmap did not become useless; it moved off the hot path. `next_up`/`next_down` are now called
from exactly one place — the narrowing branch of `on_level_emptied` — and only when the touch level
itself empties. A cancel away from the touch costs one comparison. Both accessors and the matching
cursor read an index.

Result: 25.4 → 15.0 ns, against v1's 14.3 ns. The remaining ~0.7 ns is about two cycles, roughly the
sentinel comparison the loop now carries.

### 4. Removing the same scan was worth 10.4 ns on one path and 3.9 ns on another

v2 → v2.1 deleted one thing: the `next_up(sell_occupied_, 0)` scan at the top of `submit`. The
crossing path saved **10.4 ns**; the resting path saved **3.9 ns**. Same scan, same 32 words, same
256 L1-resident bytes.

(The resting path paid for that scan at all because v2 performed it *before* discovering the order
could not cross. A non-crossing buy scanned 32 words, failed the `cur <= idx` test, and rested —
having searched for a touch it never used.)

**Hypothesis — not a measured result.** The resting path performs two heap allocations, which are
long-latency and carry no data dependency on the bitmap scan. An out-of-order core can overlap them,
so most of the scan's cost was absorbed into time the operation was already going to spend waiting.
The crossing path costs ~14 ns in total and has nothing long-latency to hide behind, so the scan sat
exposed on the critical path and its removal recovered nearly all of it.

> The same code is cheaper inside a busy function than inside an idle one, because a busy function
> has spare execution slots.

This is labelled a hypothesis because it was reasoned, not measured — `rdtsc` has no ARM equivalent
and hardware counters are awkward to reach on this platform. It makes a falsifiable prediction that
costs nothing to check later: once the resting path is allocation-free (intrusive list over a
pre-allocated pool, per finding 1), there will be nothing left to hide behind, and re-introducing
the scan should then cost close to the full ~10 ns rather than ~4 ns. If it still costs ~4 ns, this
explanation is wrong.

---

## What this leaves

**Net position.** v2.1 beats v1 on both paths that touch the price container — `cancel` by 38% at
depth 1000 and degrading a third as fast with depth, the resting path by 22% — and reaches parity on
the crossing path. No path is worse than v1.

**Known limitations, stated rather than hidden:**

- **The price window is fixed** at $79.52–$120.47 on a one-cent grid. Prices outside it, or off the
  grid, are rejected. A real venue rebases the window as the market moves; this version does not.
- **`submit` returning 0 is ambiguous** — it means "filled completely" *or* "refused". Worth fixing
  before the book is wired to anything that has to answer a participant.
- **Allocation is untouched.** All three versions `malloc` twice per resting order. Finding (1) says
  this is the dominant remaining term in `cancel`'s depth curve; finding (2) says it dominates the
  resting path outright.
- **Single symbol, single thread, no risk checks, no feed publication.** This is a book, not a
  matching engine.

**Next measurement.** Intrusive free list over a pre-allocated order pool, removing both `malloc`s
from the resting path. Prediction, stated before the change: the resting path drops sharply — it is
~57 ns of which the allocations are most — and `cancel`'s depth curve flattens further, because the
nodes it chases stop being scattered across the heap. Finding (4)'s falsifiable side-prediction rides
along with it.
