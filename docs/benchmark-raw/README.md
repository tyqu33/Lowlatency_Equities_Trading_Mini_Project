# Raw benchmark output

Unedited stdout from `hft_benchmarks`, kept so the tables in the `BENCHMARK-*.md` files can be
checked against what the tool actually printed.

Each file records one run: date, machine, cache sizes, load average, and every counter, including
the ones the write-ups do not quote.

```
./build/bin/hft_benchmarks --benchmark_filter='OrderBook' --benchmark_min_time=0.5s
```

| file | contents |
|---|---|
| `orderbook-v1-v2.1-v3-2026-08-30.txt` | three books, batch size 64: `OrderBook` (v1, `std::map`), `ArrayOrderBook` (v2.1, tick-indexed array + bitmap + cached touch), `PoolOrderBook` (v3, + pooled intrusive orders) |
| `orderbook-v1-v2.1-v3-v4-2026-09-02.txt` | all four, batch size 128, with per-benchmark `quant_err_pct`. Adds `FlatOrderBook` (v4, + open-addressed ref table). The batch size and the error counter both changed for this run; see BENCHMARK-orderbook-v4.md |

A number quoted in a write-up but absent here was measured on a run that was not kept. The v1 and v2
figures in `BENCHMARK-orderbook-v2.md` are in that position; only their conclusions survive, which
is a reason to keep these files from here on.
