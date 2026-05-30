# ADR-0006: Two-sided design — simulated exchange (venue) + trading participant, split at an OUCH/ITCH protocol boundary

- **Status:** Accepted
- **Date:** 2026-05-30
- **Context tags:** topology, market-structure, scope, fidelity
- **Supersedes framing in:** ADR-0001, ADR-0003 (revised); earlier single-sided "TE = OMS + matching" framing

## Context

An early version of this design fused order management and matching into one "Trade Engine (TE)"
and labeled it the analog of the author's Hundsun **UFT**. That conflated two distinct roles:

- **Matching does not happen inside a broker/participant system.** It happens at the **exchange**.
- The author's production experience (UFT / UFR / reference-data node, and the `src` non-fast core)
  is **entirely participant/broker side**: route orders to the exchange, gate them through risk,
  consume the exchange's reference data — **no local matching**.

So the matching engine corresponds to the **exchange**, a role the author never built, while
UFT/UFR/refdata map to the **participant**. A single-sided "TE with matching = UFT" framing is
market-structurally wrong and also misrepresents the author's experience.

A matching engine is still worth building: it is the canonical low-latency/LOB showcase, it is the
counterparty venue a participant must trade against, and it is the core of any backtester/market
simulator. The resolution is not to drop it but to put it where it belongs.

## Decision

Split the system into **two sides** communicating across a realistic protocol boundary:

- **VENUE side (① exchange):** `ME` (matching engine + central book + order acceptor) and `MDP`
  (ITCH market-data publisher).
- **PARTICIPANT side (②③ broker + buy-side/HFT):** `FH` (feed handler that rebuilds a local book
  from ITCH), `STRAT` (strategy), `RISK` (SEC 15c3-5 pre-trade risk ≈ **UFR**), `OG` (order gateway
  + OMS, sends OUCH ≈ **UFT**).
- **Protocol boundary:** **OUCH** (participant → venue order entry) and **ITCH** (venue →
  participant market data). In reality this is a network (OUCH/SoupBinTCP/TCP, ITCH/MoldUDP64/UDP
  multicast). **V1** carries it over shared-memory rings; **V3** networks it with kernel bypass.
- **15c3-5 risk is participant-side**, in front of `OG`, not inside `ME`.

## Rationale

- **Market-structurally correct**, and it maps the author's real experience precisely: `OG`≈UFT,
  `RISK`≈UFR, `FH`≈reference-data node; `ME`/`MDP` = the exchange (a new, prized skill).
- **Strongest portfolio story:** "I built both a matching venue and a trading participant, and the
  protocol boundary between them" demonstrates exchange tech, participant tech, *and* the
  systems-level understanding of where the network/latency boundary sits — exactly what HFT/prop
  firms screen for.
- **Enables the key HFT metric, tick-to-trade** (ITCH-in → strategy → OUCH-out), measurable
  end-to-end on the participant side.
- **Two books on purpose:** the venue's authoritative book vs the participant's latency-delayed
  local reconstruction (queue-position uncertainty) is the heart of HFT.
- The matching engine is preserved as the venue (showcase + counterparty + backtest core).

## Consequences

- **+** Faithful, complete, and directly job-relevant; gives `tick-to-trade` and `order→ack` as two
  distinct, honest latency figures.
- **+** Clean evolution path: V3's kernel-bypass work has genuine motivation (network the boundary).
- **−** More processes and rings; bigger MVP (≈5–7 weeks instead of 4–6). Mitigated by allowing
  process folding in MVP and phasing strategy/FIX/HA to later milestones.
- **−** Requires defining and maintaining two protocol surfaces (OUCH, ITCH) instead of one
  internal message set — but these are real, documented specifications (a plus for the résumé).
- **→** Revises ADR-0001 (now: venue book authoritative + participant local book) and ADR-0003
  (risk is participant-side, `OG`→`RISK`).
