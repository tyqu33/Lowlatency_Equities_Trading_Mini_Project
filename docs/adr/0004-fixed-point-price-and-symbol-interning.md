# ADR-0004: Fixed-point integer prices and interned integer symbol IDs

- **Status:** Accepted
- **Date:** 2026-05-30
- **Context tags:** data-model, determinism, hot-path

## Context

Prices and symbols appear in every hot-path message and in every order-book operation. The
representation choice affects determinism, comparison cost, message size, and cache behavior.

US equities have a sub-penny rule (Rule 612): minimum increment $0.01 for stocks priced ≥ $1.00,
with sub-penny pricing permitted below $1.00. Floating point would introduce rounding
non-determinism and slower comparisons; symbol strings would mean string hashing/compares on the
hot path and variable-length messages.

## Decision

- **Prices:** `int64_t` fixed-point in units of **1/10000 USD** (4 implied decimal places). This
  covers sub-penny increments, gives exact integer arithmetic and comparison, and keeps tick
  validation a pure integer modulo.
- **Symbols:** interned to a `uint32_t symbol_id` at session/admission time via the shared
  `refdata` symbol master (`symbol_id ↔ ticker`). The hot path only ever sees the integer id;
  ticker strings live in `refdata` and tooling.

## Rationale

- Determinism: integer math is exact and reproducible across runs and machines — essential for a
  matching engine and for reproducible tests.
- Speed: integer compare/branch beats float; `uint32` symbol id makes book lookup an array/hash
  index, not a string compare.
- Compactness: fixed-width fields keep messages fixed-size, which suits fixed-size ring slots and
  cache-line alignment.

## Consequences

- **+** Exact, fast, fixed-size; trivial tick-size validation.
- **+** Enables array-indexed price levels and integer-keyed order maps.
- **−** Must centralize scale (1e4) and never mix scales; a thin `Price` wrapper type is advisable
  to prevent raw-int confusion.
- **−** Symbol interning requires a populated `refdata` master before order admission (a startup
  ordering constraint).
