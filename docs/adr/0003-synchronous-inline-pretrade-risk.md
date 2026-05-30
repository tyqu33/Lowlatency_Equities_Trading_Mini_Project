# ADR-0003: Synchronous inline pre-trade risk on the participant side (OG calls RISK and waits)

- **Status:** Accepted
- **Date:** 2026-05-30 (revised after ADR-0006: risk is participant-side, not in the venue)
- **Context tags:** risk, 15c3-5, critical-path, latency, participant-side

## Context

SEC Rule 15c3-5 (the Market Access Rule) is a **broker/participant** obligation: pre-trade
financial and regulatory controls applied on an **automated, pre-trade basis** — *before* an order
is routed to the exchange. It is therefore on the **participant side**, in front of the order
gateway — **not** inside the venue's matching engine (orders reaching the exchange have already
passed the broker's 15c3-5 controls).

Two ways to wire it:
1. **Synchronous inline:** `OG` sends a `RiskRequest`, spin-waits for the `RiskResponse`, and only
   then routes the order (OUCH) to the venue.
2. **Asynchronous pipelined:** `OG` forwards to risk and continues; a per-order state machine
   routes once the verdict arrives, allowing overlap.

The author's production background (Hundsun UFT calling UFR, the ultra-fast risk node) uses the
synchronous call model: UFT issues a request and waits for the verdict before releasing the order.

## Decision

Use **synchronous inline** risk for V1, on the participant side. `OG` sends `RiskRequest` on
`risk_req`, busy-waits on `risk_resp`, and routes to the venue only on `Approve`. The risk
round-trip is deliberately **on the critical path** and is measured/published separately.

## Rationale

- Faithful to 15c3-5 "pre-trade, automated" semantics, to where the rule actually applies (the
  broker/participant), and to the real **UFT→UFR** call model (`OG` ≈ UFT, `RISK` ≈ UFR).
- Deterministic, in-order handling — simple to reason about and test.
- Honest: rather than hiding risk cost behind async overlap, we expose and measure it. "I measured
  what a compliant pre-trade risk check costs on the hot path, and here's the number" is a stronger
  story than pretending it's free.

## Consequences

- **+** Simple, deterministic, regulation-faithful; clean to unit test.
- **+** Concrete, defensible latency datapoint (risk round-trip µs) that also feeds the
  participant's **tick-to-trade** figure.
- **−** Adds an IPC round-trip to every order's critical path. Mitigated by busy-spin polling and
  cache-warm SPSC rings; quantified in benchmarks.
- **→** V2 may add the asynchronous pipelined model as an *optimization study* (before/after
  latency), but it is out of scope for V1.
