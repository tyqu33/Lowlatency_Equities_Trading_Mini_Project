# ADR-0001: The venue matching engine owns the authoritative book; the participant rebuilds a separate local book

- **Status:** Accepted
- **Date:** 2026-05-30 (revised after ADR-0006's two-sided split)
- **Context tags:** topology, market-data, book-building, simulation

## Context

Market data must be a faithful function of real matching, and a trading participant only ever sees
the venue through a (latency-delayed) feed. A real exchange's matching engine produces an
order-by-order event stream; a market-data publisher translates it into ITCH and disseminates it
(e.g., UDP multicast); participants reconstruct their own local book from that feed.

## Decision

- **`ME` (venue) owns the authoritative central order book.** It emits an order-by-order event
  stream (Add / Executed / Cancel / Delete / Trade) on the `book_evt` SPSC ring.
- **`MDP` (venue) is a pure publisher.** It consumes `book_evt`, rebuilds the L2 (5-level)
  snapshot, encodes ITCH, and broadcasts on the `md_wire` SPMC ring. It never invents book state.
- **`FH` (participant) rebuilds a *separate* local book** from the ITCH feed, with sequence/gap
  detection and snapshot resync. This local book is what the strategy sees.
- **`sim_client` is a separate participant** that injects OUCH orders into the venue to create
  realistic book depth — "participants make the market, the venue keeps the truth, MD publishes it,
  and every participant reconstructs its own view."

## Consequences

- **+** Single source of truth (the venue book); MD is a pure function of the event stream.
- **+** Two books *on purpose*: the gap between the venue's truth and the participant's
  latency-delayed local book (queue-position uncertainty) is the essence of HFT and a deliberate
  teaching point.
- **+** Mirrors the real exchange (matching → MD publisher) ↔ participant (feed handler → local
  book) pipeline.
- **+** `sim_client` can be swapped for real strategy processes with no architectural change.
- **−** Gap/torn-read handling needed on the broadcast ring (mitigated by L2 snapshot resync).
- **−** The participant must reconcile its local book against fills it receives via `exec_wire`.
