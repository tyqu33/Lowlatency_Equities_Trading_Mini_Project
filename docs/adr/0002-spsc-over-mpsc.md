# ADR-0002: SPSC point-to-point + SPMC broadcast; no MPSC

- **Status:** Accepted
- **Date:** 2026-05-30
- **Context tags:** ipc, lock-free, queues

## Context

The system needs inter-process queues for: order entry (multiple sources), risk request/response,
execution reports, the matching-engine event stream, and market-data fan-out. The naive instinct
is a shared multi-producer queue (MPSC) for order entry and a multi-consumer queue for market data.

## Decision

- **Every point-to-point link is SPSC.** `order_in_*`, `risk_req`, `risk_resp`, `exec_out`,
  `book_evt` are all single-producer/single-consumer.
- **Multiple order sources → one SPSC ring per session.** TE multiplexes by polling its set of
  inbound rings. We do **not** fold them into a shared MPSC ring.
- **Market-data fan-out → one SPMC broadcast ring.** A single overwriting producer; each consumer
  keeps its own read cursor and never blocks the producer; gaps are detected via per-slot sequence
  and recovered from the latest L2 snapshot.
- **No MPSC anywhere.**

## Rationale

- SPSC is the fastest lock-free queue: no CAS, no retry loops, only acquire/release loads/stores.
  The single-writer principle makes correctness obvious and cache behavior optimal (with cursor
  caching à la LMAX Disruptor, each side mostly touches its own cache line).
- Per-session SPSC rings give natural isolation, backpressure, and rate-limiting boundaries —
  exactly how real gateways shard client sessions — and avoid the CAS contention and tail-latency
  inflation of MPSC.
- SPMC broadcast matches the drop-and-recover semantics of exchange UDP-multicast market data.
- The cost (TE polling N rings) is negligible for a small N and is dwarfed by the win in
  simplicity and determinism.

## Consequences

- **+** Lowest-latency, simplest-to-verify queue primitives.
- **+** Topology "dissolves" MPSC into N×SPSC — a defensible design talking point.
- **−** TE must fairly poll multiple inbound rings (round-robin / weighted); a fairness policy is needed.
- **−** Broadcast ring requires torn-read/gap handling on the consumer side.
