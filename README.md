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

> **Status: in progress — building bottom-up.** The shared foundation is implemented and tested;
> the components above it are being written on top of it, in the order given by the roadmap below.
>
> | | |
> |---|---|
> | **Implemented, measured, under test** | `hft::matching` — price-time-priority limit order book: add / cancel / match with partial fills, O(1) cancel, fills priced at the resting order. **Three interchangeable implementations** — a `std::map` baseline; a tick-indexed array with a per-side occupancy bitmap and an incrementally-maintained touch; and that plus pooled, intrusively-linked orders — all run against the same 28-case typed test suite, which is what proves they behave identically. `hft::common` — fixed-point `Price` (tick arithmetic, parse/format, floor/ceil-to-tick) and power-of-two/alignment helpers. 112 GoogleTest cases, including property-based sweeps over the rounding invariants and a randomised sweep asserting the book never crosses. Builds clean with warnings-as-errors; ASan/UBSan and GitHub Actions CI wired up. |
> | **In progress** | Replacing the `unordered_map` ref index with direct indexing, to remove the last per-order heap allocation — the term the benchmarks below identify as dominant in what remains. |
> | **Designed, not yet built** | `MDP` / `FH` / `STRAT` / `RISK` / `OG`, the OUCH+ITCH protocol boundary, and the shared-memory transport. Interfaces and wire structs are in place; bodies are stubs. |
>
> See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design and
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
# Configure (first run fetches fmt / toml++ / CLI11 via FetchContent, then cached):
cmake -S . -B build
cmake --build build -j
./build/bin/hft_config_demo --config config/venue.toml -v   # tooling smoke test

# See the order book work: replay a script of orders and print the book after each one.
./build/bin/hft_book_demo tools/book_demo/sample-orders.txt
./build/bin/hft_book_demo tools/book_demo/sample-orders.txt --book map    # or: --book pool

# With tests (GoogleTest + GoogleMock) and benchmarks (Google Benchmark):
cmake -S . -B build -DHFT_BUILD_TESTS=ON -DHFT_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/bin/hft_benchmarks
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
│   ├── protocol/            #   hft::protocol  — OUCH / ITCH / risk wire structs (header-only)
│   ├── matching/            #   hft::matching  — price-time-priority limit order book
│   └── appkit/              #   hft::appkit    — CLI (CLI11) + TOML (toml++) + fmt plumbing
├── apps/
│   ├── venue/{me,mdp}/       # VENUE: matching engine, market-data publisher
│   └── participant/{fh,strat,risk,og}/   # PARTICIPANT: feed handler, strategy, risk, gateway
├── tools/                   # book_demo (replay orders, print the book) + sim_client + config_demo
├── tests/                   # GoogleTest + GoogleMock unit tests
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

### Order book — three implementations, one workload

Per-operation p50 in nanoseconds at 1000 price levels per side, batch-averaged. **v2** replaced v1's
`std::map` price levels with a tick-indexed array + occupancy bitmap, **v2.1** added an incrementally
maintained touch, and **v3** moved resting orders into a pre-allocated pool with intrusive links.
Same seeded workload, same fixture, one variable changed at a time.

| Operation | v1 (`std::map`) | v2.1 (array + bitmap + cached touch) | v3 (+ pooled intrusive orders) |
|---|---:|---:|---:|
| `cancel` | 133.5 ns | 82.0 ns | **52.1 ns** (−61%) |
| `submit` — rests | 71.6 ns | 56.6 ns | **39.1 ns** (−45%) |
| `submit` — crosses | 14.3 ns | 15.0 ns | **11.7 ns** (−18%) |

`cancel`'s sensitivity to book depth over a 100x range falls from **+99%** to **+30%**, and its
p99.9 from 1964 ns to **109 ns**.

Two results are worth more than the improvements. **v2 regressed the matching path by 91%** — the
array rescanned its bitmap from index 0 on every order, where `std::map` gets its leftmost node
cached for free — and v2.1 exists to fix exactly that. And the measurement had to be established
before it could be trusted: this platform's clock has a **41.67 ns** granularity, which makes
per-operation timing meaningless at these magnitudes, so operations are timed in batches of 64
(quantisation error 47% → 0.71%).

Predictions were written down before each change. Eight so far, of which three were wrong and one
half right; the write-ups say which, and what the wrong ones revealed. Each version changes exactly
one thing, so the delta is attributable, and the raw benchmark output is kept alongside the analysis.

- **[`docs/BENCHMARK-orderbook-v1.md`](docs/BENCHMARK-orderbook-v1.md)** — baseline, method, and the
  predictions made before v2 was written
- **[`docs/BENCHMARK-orderbook-v2.md`](docs/BENCHMARK-orderbook-v2.md)** — the tick-indexed array,
  the 91% regression and its diagnosis, and the cached touch that fixed it
- **[`docs/BENCHMARK-orderbook-v3.md`](docs/BENCHMARK-orderbook-v3.md)** — pooled intrusive orders;
  why the depth curve dropped without flattening, and why a path that never allocates got faster
- **[`docs/benchmark-raw/`](docs/benchmark-raw)** — unedited tool output behind the tables

Measured on a MacBook Air (Apple Silicon) under ordinary desktop load — not tuned bare metal. **The
honest claim from a run like this is a relative improvement under identical conditions, never an
absolute production latency figure.**

### Still to be measured

The three metrics this project exists to measure, once the corresponding components are built:

- **tick-to-trade** — ITCH-in → strategy decision → OUCH-out (the participant-side headline number)
- **venue order → ack** — OUCH-in → match → ack-out
- **SPSC ring round-trip** — the single-hop IPC cost that bounds both of the above

Each will be reported as p50 / p99 / p99.9 from HdrHistogram, alongside the load that generated it.

## License

TBD (add a `LICENSE` file before publishing — MIT or Apache-2.0 recommended).
