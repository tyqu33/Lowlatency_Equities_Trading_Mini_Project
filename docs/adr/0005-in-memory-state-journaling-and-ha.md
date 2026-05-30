# ADR-0005: All-in-memory state, journal-based persistence, active-standby HA

- **Status:** Accepted
- **Date:** 2026-05-30
- **Context tags:** in-memory, persistence, recovery, high-availability, pre-allocation

## Context

Low-latency trading cores keep **all state in RAM** — a single database round-trip (milliseconds)
is disqualifying on a µs-class hot path. This is universal practice (NASDAQ INET, LSE Millennium,
CME, LMAX Exchange), not market-specific. The author's prior production system (Hundsun UFT) used
an all-in-memory + primary/backup design and required 64–128 GB per machine — because pools are
pre-allocated for whole-market peak (plus the standby mirror and journal buffers) **up front,
regardless of load**, so even an idle test box must own that RAM (a 16 GB machine could not run it).

The original design implied memory residency (shm rings, pre-allocated order pool, no DB on the
hot path) but never made it a first-class tenet, and had **no persistence/recovery story and no
high-availability story** — a real gap relative to production systems.

## Decision

1. **All-in-memory is a stated tenet.** Memory is the system of record; no database on the hot
   path. The book, reference master, positions/limits, live orders, and all message/order pools
   live in RAM.
2. **Memory discipline:** startup pre-allocation of all pools (no runtime `malloc`/`free` on the
   hot path), `mlockall` + page pre-touch, huge pages, NUMA-local binding.
3. **Persistence via event sourcing:** an append-only, `mmap`-backed input-event journal keyed by
   ring `seq_no` is the durable source of truth; periodic state snapshots bound replay time;
   recovery = load latest snapshot + replay the journal tail. Deterministic single-writer logic
   guarantees an identical rebuild.
4. **High availability via active-standby:** a warm standby replays the primary's shipped journal
   to maintain a hot in-memory mirror and is promoted on primary failure — faithful to the UFT
   primary/backup model. (Doubles the memory requirement.)

## Scope / phasing

- **V1:** all-in-memory + pre-allocation discipline in code; HA and journaling *documented only*.
- **V2:** implement the input journal (event sourcing) + snapshots + replay-on-startup recovery;
  make `mlockall`/huge-pages/pre-touch explicit.
- **V3:** ship a *simplified journal-shipping warm-standby demo* (backup tails the primary's
  journal, can take over). Full consensus (Raft / Aeron Cluster) is explicitly **out of scope** —
  it is the production answer, but overkill for a learning project.

## Consequences

- **+** Faithful to real HFT cores; a strong, distinct interview topic (LMAX-aligned).
- **+** Deterministic replay gives both crash recovery and an exact HA mirror "for free."
- **+** Configurable pool sizing lets the simulator run on a laptop yet explains 64–128 GB at scale.
- **−** Journaling adds an off-hot-path write per input event; must stay asynchronous / batched so
  it never stalls the critical path.
- **−** Active-standby roughly doubles memory; promotion/fencing logic (split-brain avoidance) is
  subtle even in the simplified demo.
- **→** Relies on the determinism guaranteed by ADR-0004 (fixed-point) and the single-writer
  principle of ADR-0002.
