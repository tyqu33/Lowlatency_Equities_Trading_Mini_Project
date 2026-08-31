# OrderBook v3 — pooled intrusive orders, measured against v1 and v2.1

`hft::matching::PoolOrderBook` keeps everything v2.1 arrived at — tick-indexed price levels, a
per-side occupancy bitmap, an incrementally maintained touch — and changes one thing: resting orders
come from a pre-allocated pool and carry their own list links, instead of being allocated one at a
time inside a `std::list`.

That removes **one of the two heap allocations per resting order**. The other one, the
`std::unordered_map` node in the ref index, is deliberately left in place so the result stays
attributable.

This was not a guess about what to optimise next. It is the cost the previous two rounds of
measurement identified, twice, from opposite directions — finding (2) in the v1 write-up and finding
(1) in the v2 write-up both landed on allocation.

Raw output: [`benchmark-raw/orderbook-v1-v2.1-v3-2026-08-30.txt`](benchmark-raw/orderbook-v1-v2.1-v3-2026-08-30.txt).
Reproduce with

```
cmake -S . -B build -DHFT_BUILD_BENCHMARKS=ON && cmake --build build -j
./build/bin/hft_benchmarks --benchmark_filter='OrderBook' --benchmark_min_time=0.5s
```

---

## Method

Unchanged from v1 and v2 in every respect that could move a number: same seeded workload, same book
shape, same batch-of-64 timing, same machine. See the v1 write-up for why the workload has that
shape and why operations are timed in batches.

**One run, all three books.** v2.1's measurement had to compare across two runs, because it replaced
v2 in place. v3 is a separate class, so all three implementations ran in the same process, minutes
apart at most, against a byte-identical fixture. The caveat that write-up had to carry does not
apply here.

**The fixture still asserts it was populated.** All three books reject prices outside their window,
and `PoolOrderBook` also rejects orders when its pool is exhausted. A book that silently refused the
workload would look extremely fast and nothing in the timing loop would notice, so `order_count()`
is checked against the expected count after every rebuild. The deepest fixture rests 20 000 orders
against a pool of 32 768.

Clock granularity measured at 41 ns; batching 64 operations puts quantisation error at **0.71%**.

**Environment.** MacBook Air (Apple Silicon, 8 cores, 64 KiB L1d, 4 MiB L2), macOS, ordinary desktop
load, load average ~1.8. Not tuned bare metal. **The honest claim from a run like this is a relative
improvement under identical conditions, never an absolute production latency figure.**

---

## Results

Per-operation nanoseconds, batch-averaged. **Bold** marks the best of the three.

### `cancel`

| metric | depth | v1 (`std::map`) | v2.1 (array + touch) | v3 (+ pool) |
|---|---:|---:|---:|---:|
| p50 | 10 | 67.1 | 62.5 | **32.6** |
| | 100 | 95.0 | 69.0 | **40.4** |
| | 1000 | 133.5 | 82.0 | **52.1** |
| p99 | 1000 | 227.2 | 134.8 | **68.4** |
| p99.9 | 1000 | 1964.2 | 1724.0 | **109.4** |
| ops/s | 1000 | 7.02 M | 11.40 M | **19.25 M** |

### `submit` — the resting path (no cross)

| metric | depth | v1 | v2.1 | v3 |
|---|---:|---:|---:|---:|
| p50 | 10 | 74.9 | 57.9 | **42.3** |
| | 100 | 72.9 | 57.3 | **40.4** |
| | 1000 | 71.6 | 56.6 | **39.1** |
| p99 | 1000 | 102.2 | 85.3 | **47.5** |
| ops/s | 1000 | 13.00 M | 16.23 M | **28.52 M** |

### `submit` — the crossing path (fully filled, rests nothing)

| metric | depth | v1 | v2.1 | v3 |
|---|---:|---:|---:|---:|
| p50 | 10 | 14.3 | 15.0 | **11.7** |
| | 100 | 13.7 | 15.0 | **11.7** |
| | 1000 | 14.3 | 15.0 | **11.7** |
| p99.9 | 1000 | 23.4 | 22.8 | **16.3** |
| ops/s | 1000 | 67.10 M | 63.90 M | **83.41 M** |

### Net position at depth 1000

| operation | v1 | v3 | Δ vs v1 | Δ vs v2.1 |
|---|---:|---:|---:|---:|
| `cancel` | 133.5 ns | **52.1 ns** | **−61.0%** | −36.5% |
| `submit` (rests) | 71.6 ns | **39.1 ns** | **−45.4%** | −31.0% |
| `submit` (crosses) | 14.3 ns | **11.7 ns** | **−18.2%** | −21.7% |

Every path is now faster than v1, including the one v2 regressed by 91%.

---

## Predictions and outcomes

The v2 write-up committed two predictions before v3 was written.

| prediction | outcome |
|---|---|
| The resting path drops sharply; it is ~57 ns of which the allocations are most | **Held.** −17.5 ns (−31%) from removing one of the two. |
| `cancel`'s depth curve flattens further, because the nodes it chases stop being scattered across the heap | **Wrong.** The curve did not flatten. It moved down by a constant and kept its slope exactly. |

Two predictions were also made and not tested. Finding (4)'s falsifiable side-prediction — that once
the resting path is allocation-free, re-introducing v2's bitmap scan should cost the full ~10 ns
rather than ~4 ns — **cannot be evaluated yet**, because the resting path still performs one
allocation. It carries forward to the next round.

---

## Analysis

### 1. The depth curve did not flatten. It translated.

This is the most informative number in the run.

| | p50 at depth 10 | p50 at depth 1000 | growth |
|---|---:|---:|---:|
| v2.1 | 62.50 ns | 82.03 ns | **+19.53 ns** |
| v3 | 32.56 ns | 52.08 ns | **+19.52 ns** |

The whole curve dropped by ~30 ns and the slope stayed put.

The prediction assumed that pooling the order nodes would remove the scattered-memory effect that
makes `cancel` depth-sensitive. It removed *part* of it. What remains has a specific address:

`cancel` performs **no allocation at all** — it is `refMap.find`, `unlink`, `release`,
`refMap.erase`. So its depth sensitivity was never about the cost of allocating. It is about
**where the allocated objects ended up**:

| depth | resting orders | `refMap` nodes + buckets | live pool span |
|---:|---:|---|---|
| 10 | 200 | ~15 KB | 6 KB |
| 1000 | 20 000 | **~1.5 MB** | 640 KB |

v3 moved the order nodes into a contiguous 1 MiB array, so the hop from a pool index to an order is
now a bounded stride into a region that at least *has* locality. The hop from a reference number to
a pool index still goes through a hash node that `malloc` placed wherever it liked, and the set of
those nodes still grows to ~1.5 MB at depth 1000.

> This is the third time the same sentence explains a result: **O(1) says how many times you jump,
> not how far.** In v1 it explained the tree; in v2 it explained why the array did not flatten; here
> it explains why the pool did not either. The cost that keeps surviving is the one structure nobody
> has touched.

A caveat worth stating: 19.53 vs 19.52 is a single run, and the pool itself (640 KB live at depth
1000) and the levels arrays (64 KB per side) also grow their working sets. `refMap` is the largest
remaining scattered structure, not the only one. The agreement is probably partly coincidence.

### 2. One allocation removed is worth ~17.5 ns; the other is probably worth about the same

The resting path lost a flat ~17.5 ns at every depth (57.9 → 42.3, 57.3 → 40.4, 56.6 → 39.1). It
still allocates one `unordered_map` node. If the two allocations cost roughly the same, then of v3's
remaining ~39 ns, roughly 17 ns is still allocation, and the actual work of resting an order is
around 22 ns.

That is an estimate with an obvious test attached: replacing `refMap` should take off another ~17 ns.
If it takes off 5 ns, the two allocations are not comparable and this arithmetic is wrong. Cheap to
check, and it is the next step anyway.

### 3. The crossing path got faster too — and it never allocates

This is the result that was not predicted, and the one worth understanding.

The crossing benchmark submits orders that fill completely and rest nothing. `alloc()` is never
called on that path. Removing allocation should have done nothing to it. It went from 15.0 ns to
11.7 ns, a 22% improvement, and is now 18% faster than v1.

The reason is that **deallocation is a cost too, and it lands on the consuming path.** When a
crossing order eats a resting order completely, that order has to go away:

| | consuming a resting order |
|---|---|
| v2.1 | `orders.pop_front()` — destroys the node, calls the allocator's deallocate, `operator delete`, `free` underneath |
| v3 | `release(slot)` — two assignments |

An operation that allocates nothing can still be paying for allocation, because someone has to give
the memory back. Only the path that hands out memory is obvious; the path that takes it back is easy
to overlook precisely because it does not look like allocation.

An asymmetry falls out of the two numbers:

| | saved |
|---|---:|
| resting path, one **allocation** removed | 17.5 ns |
| crossing path, one **deallocation** removed | 3.3 ns |

Allocation appears to be roughly five times more expensive than deallocation here, which is
consistent with how general-purpose allocators work — finding a suitable block is a search, giving
one back is usually a push. Stated as an observation rather than a conclusion: the two numbers come
from two different paths, so this is not a controlled comparison.

### 4. The tail improved far more than the median

`cancel` at depth 1000:

| | p50 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|
| v2.1 | 82.0 | 134.8 | **1724.0** | 3095 |
| v3 | 52.1 | 68.4 | **109.4** | 1950 |

The median improved 1.6x. The p99.9 improved **16x**.

The v1 write-up attributed that tail to the operating system — a preempted thread or a page fault —
on the grounds that a red-black tree rebalance is three orders of magnitude too cheap to explain a
110 µs batch. That reasoning still holds for the *maximum*, which is the same order of magnitude in
both (3.1 µs vs 1.9 µs, and both are batch means, so the underlying single-call excursions are much
larger).

What changed is the *frequency* of slow batches, and that is not something the OS decides. The
plausible mechanism is the allocator: v2.1's `cancel` returns two nodes per call, v3's returns one,
and a general-purpose allocator has slow paths (coalescing, returning spans to the OS, lock
contention on a shared arena) that are rare per call but no longer rare across a batch of 64. Halving
the calls halves the chances of hitting one.

Flagged as plausible rather than demonstrated. It predicts that removing the second allocation should
collapse this tail further; that is checkable in the next round.

---

## What this leaves

**Net position.** v3 is faster than v1 on all three paths, by 61% on `cancel`, 45% on the resting
path, and 18% on the crossing path. Behaviour is identical to both earlier books: all three run the
same 28-case typed suite, 112 tests green, and `book_demo` produces byte-identical output from v2.1
and v3 on the same script.

**Known limitations, stated rather than hidden:**

- **The pool is a fixed 32 768 orders.** An order arriving when it is exhausted is rejected. Real
  venues size for a worst case and monitor headroom; this one has neither.
- **`submit` returning 0 is now ambiguous three ways** — "filled completely", "price refused",
  "pool exhausted". This was worth fixing one version ago.
- **The price window is fixed** at $79.52–$120.47 on a one-cent grid, and does not rebase.
- **One allocation per resting order remains**, in the ref index. Findings (1), (2) and (4) all point
  at it.
- **Single symbol, single thread, no risk checks, no feed publication.** This is a book, not a
  matching engine.

**Next measurement.** Replace `refMap` with direct indexing, removing the last allocation. It has a
prerequisite this version did not have to answer: whether `OrderRefNum`s are dense enough to index an
array, or whether they need a mapping layer that reintroduces the problem.

Predictions, stated before the change:

1. The resting path loses another ~17 ns, per finding (2). If it loses ~5 ns instead, that finding's
   arithmetic is wrong.
2. **`cancel`'s depth curve finally flattens** — the +19.5 ns slope that survived both previous
   rounds is, on the finding (1) reading, mostly this structure. If the slope survives a third time,
   the explanation is wrong and the cause is somewhere not yet examined.
3. The p99.9 tail collapses further, per finding (4).
4. Finding (4) of the v2 write-up becomes testable: with the resting path finally allocation-free,
   re-introducing v2's index-0 bitmap scan should cost close to the full ~10 ns rather than ~4 ns. If
   it still costs ~4 ns, the out-of-order-execution explanation given there is wrong.
