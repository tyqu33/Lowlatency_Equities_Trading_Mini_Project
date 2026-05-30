# hft — Low-Latency US-Equities Trading Stack

A modern-C++20, single-machine, multi-process, shared-memory trading stack modeled on US equities
market structure. It is **two-sided**:

- a **VENUE** (simulated exchange): a price-time **matching engine** + an **ITCH**-style market-data
  publisher;
- a **PARTICIPANT** (broker + buy-side/HFT): a **feed handler** that rebuilds a local book from the
  venue's ITCH, a **strategy**, a **SEC 15c3-5 pre-trade risk** gate, and an **OUCH** order gateway.

The two sides communicate across a realistic protocol boundary — **OUCH** for order entry, **ITCH**
for market data. Each side talks internally over hand-rolled lock-free **SPSC/SPMC** rings in shared
memory.

> **Status: skeleton.** Architecture and scaffolding are in place; component implementations are
> stubs (`// TODO`). See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design and
> [`docs/adr/`](docs/adr) for the design decisions.

---

## Why two-sided?

Matching happens at the **exchange**, not inside a broker/participant system. Modeling both — and
the network/protocol boundary between them — demonstrates exchange tech, participant tech, *and* the
systems-level understanding of where latency lives. It also yields the key HFT metric end-to-end:
**tick-to-trade** (ITCH-in → strategy decision → OUCH-out). See [ADR-0006](docs/adr/0006-two-sided-venue-and-participant.md).

```
   PARTICIPANT side (= broker / buy-side)            VENUE side (= exchange)
   ┌───────────────────────────────────┐            ┌──────────────────────────┐
   │  FH ──► STRAT ──► OG ──► RISK       │  OUCH ───► │  acceptor ──► ME (book +  │
   │  ▲local book      (= UFT) (= UFR)   │ ◄─── OUCH  │   price-time matching)    │
   │  └──────────────────────────────┐  │            │            │              │
   │           ITCH ◄────────────────┼──┼──────────  │  ME ──► MDP (ITCH + L2)    │
   └───────────────────────────────────┘            └──────────────────────────┘
```

| Component | Role | Maps to (author's UFT background) |
|---|---|---|
| `ME`  | matching engine + central order book (venue) | the exchange (SSE/SZSE) — *new skill* |
| `MDP` | ITCH market-data publisher (venue) | the exchange's market-data feed |
| `FH`  | feed handler — consume ITCH, rebuild local book | the reference-data sync node |
| `STRAT` | strategy (market maker / taker) | — |
| `RISK`  | SEC 15c3-5 pre-trade risk | **UFR** (ultra-fast risk) |
| `OG`    | order gateway + OMS (sends OUCH) | **UFT** (ultra-fast trading) |

---

## Build

```bash
# Skeleton build (no external deps, offline):
cmake -S . -B build
cmake --build build -j

# With tests (fetches GoogleTest) and benchmarks (fetches Google Benchmark):
cmake -S . -B build -DHFT_BUILD_TESTS=ON -DHFT_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Useful options: `-DHFT_ENABLE_SANITIZERS=ON` (ASan/UBSan), `-DHFT_NATIVE_ARCH=ON` (`-march=native`).
Requires CMake ≥ 3.20 and a C++20 compiler.

---

## Repository layout

```
hft/
├── CMakeLists.txt           # top-level build
├── cmake/                   # CompilerWarnings / Sanitizers / Dependencies modules
├── config/                  # example venue / participant configs (TOML)
├── docs/                    # ARCHITECTURE.md, ADRs
├── libs/                    # shared libraries
│   ├── common/              #   hft::common   — Price, symbol interning, types, time, cpu
│   ├── ipc/                 #   hft::ipc       — lock-free SPSC/SPMC rings + shared memory
│   └── protocol/            #   hft::protocol  — OUCH / ITCH / risk wire structs (header-only)
├── apps/
│   ├── venue/{me,mdp}/       # VENUE: matching engine, market-data publisher
│   └── participant/{fh,strat,risk,og}/   # PARTICIPANT: feed handler, strategy, risk, gateway
├── tools/sim_client/        # background order-flow generator
├── tests/                   # GoogleTest unit tests
└── benchmarks/              # Google Benchmark microbenchmarks
```

---

## Roadmap (summary)

- **MVP** — closed-loop trading sim (venue ⇄ participant), price-time matching, local book building,
  15c3-5 risk, HdrHistogram tick-to-trade + order→ack.
- **V2** — FIX 4.4 entry, fuller risk (Reg SHO, STP, kill switch), event-sourcing persistence,
  performance tuning, multi-symbol sharding.
- **V3** — network the protocol boundary (UDP multicast ITCH / TCP OUCH) + kernel bypass; active-standby HA.

Full plan and acceptance criteria: [`docs/ARCHITECTURE.md` §8](docs/ARCHITECTURE.md).

## Benchmarks

Measured percentiles will be published here (currently placeholders — the skeleton has no
implementation to measure):

| Metric | p50 | p99 | p99.9 |
|---|---|---|---|
| SPSC ring round-trip | TBD | TBD | TBD |
| tick-to-trade (ITCH-in → OUCH-out) | TBD | TBD | TBD |
| venue order → ack | TBD | TBD | TBD |

## License

TBD (add a `LICENSE` file before publishing — MIT or Apache-2.0 recommended).
