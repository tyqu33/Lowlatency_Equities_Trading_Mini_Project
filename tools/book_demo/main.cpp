// tools/book_demo/main.cpp
// A runnable window onto hft::matching — feed it a script of orders, watch the book change.
//
//   ./build/bin/hft_book_demo tools/book_demo/sample-orders.txt
//   ./build/bin/hft_book_demo tools/book_demo/sample-orders.txt --book map
//
// This is a demonstration tool, not a component of the system: nothing here is on a hot path and
// nothing here is benchmarked. Its job is to make the book's behaviour visible — price-time
// priority, fills printing at the RESTING order's price, partial fills keeping their place in line
// — without requiring the reader to run a test suite and infer it from assertions.
//
// It runs against EITHER implementation, selected by --book, because both expose the same
// interface. That is the same property tests/test_order_book.cpp relies on, made visible: the same
// script through both books must produce the same output.
//
// ---------------------------------------------------------------------------
// SCRIPT FORMAT
// ---------------------------------------------------------------------------
//
//   BUY   <qty> <price>     submit a Day limit buy  — the reference is assigned automatically
//   SELL  <qty> <price>     submit a Day limit sell
//   CANCEL <ref>            cancel a previously submitted order by its assigned reference
//   # ...                   comment; blank lines are ignored
//
// Prices are parsed by hft::common::Price::from_string, so anything that accepts is legal here:
// "100", "100.02", "99.995". The book display walks in one-cent steps, so off-cent prices will
// rest correctly but may not appear in the ladder.

#include <fmt/color.h>
#include <fmt/core.h>

#include <CLI/CLI.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/array_order_book.hpp"
#include "hft/matching/order_book.hpp"
#include "hft/matching/flat_order_book.hpp"
#include "hft/matching/pool_order_book.hpp"

namespace {

using hft::common::OrderRefNum;
using hft::common::OrderType;
using hft::common::Price;
using hft::common::Qty;
using hft::common::Side;
using hft::common::TimeInForce;
using hft::matching::ArrayOrderBook;
using hft::matching::Fill;
using hft::matching::OrderBook;
using hft::matching::OrderRequest;
using hft::matching::FlatOrderBook;
using hft::matching::PoolOrderBook;

constexpr std::int64_t kCent = 100;        // one cent, in Price's 1/10000 USD ticks
constexpr std::size_t kDepthShown = 5;     // ladder rows per side
constexpr int kLadderScanLimit = 200;      // bound the walk on a sparse book

// ---- Book display ----------------------------------------------------------

// Collects up to kDepthShown occupied levels by walking outward from the touch in one-cent steps.
// Uses only the public interface (best_bid / best_ask / qty_at), so it works unchanged against both
// implementations — a level enumerator would have to be added to both, and neither needs one.
template <typename Book>
std::vector<std::pair<Price, Qty>> ladder(const Book& book, Side side) {
    std::vector<std::pair<Price, Qty>> out;
    const auto touch = (side == Side::Buy) ? book.best_bid() : book.best_ask();
    if (!touch) {
        return out;
    }
    const std::int64_t step = (side == Side::Buy) ? -kCent : kCent;
    std::int64_t ticks = touch->ticks();
    for (int i = 0; i < kLadderScanLimit && out.size() < kDepthShown; ++i, ticks += step) {
        const Price p = Price::from_ticks(ticks);
        const Qty q = book.qty_at(side, p);
        if (q > 0) {
            out.emplace_back(p, q);
        }
    }
    return out;
}

template <typename Book>
void print_book(const Book& book) {
    const auto bids = ladder(book, Side::Buy);
    const auto asks = ladder(book, Side::Sell);

    fmt::print("      {:>22}  │  {:<22}\n", "BID", "ASK");
    const std::size_t rows = std::max(bids.size(), asks.size());
    if (rows == 0) {
        fmt::print("      {:>22}  │  {:<22}\n", "(empty)", "(empty)");
    }
    for (std::size_t r = 0; r < rows; ++r) {
        std::string left = "-";
        std::string right = "-";
        if (r < bids.size()) {
            left = fmt::format("{} @ {}", bids[r].second, bids[r].first.to_string());
        }
        if (r < asks.size()) {
            right = fmt::format("{} @ {}", asks[r].second, asks[r].first.to_string());
        }
        fmt::print("      {:>22}  │  {:<22}\n", left, right);
    }

    if (const auto bb = book.best_bid()) {
        if (const auto ba = book.best_ask()) {
            fmt::print(fmt::fg(fmt::color::gray), "      spread {} ticks · {} orders resting\n",
                       ba->ticks() - bb->ticks(), book.order_count());
            return;
        }
    }
    fmt::print(fmt::fg(fmt::color::gray), "      {} orders resting\n", book.order_count());
}

// ---- Script execution ------------------------------------------------------

struct Totals {
    std::size_t submitted{};
    std::size_t cancelled{};
    std::size_t fills{};
    Qty volume{};
};

template <typename Book>
bool run_script(std::istream& script, const std::string& label) {
    Book book;
    std::vector<Fill> fills;
    OrderRefNum next_ref = 0;
    Totals totals;

    fmt::print(fmt::emphasis::bold, "\nbook_demo — {}\n", label);

    std::string line;
    int line_no = 0;
    while (std::getline(script, line)) {
        ++line_no;
        std::istringstream in(line);
        std::string verb;
        if (!(in >> verb) || verb[0] == '#') {
            continue;
        }

        // ---- CANCEL <ref> ----
        if (verb == "CANCEL" || verb == "cancel") {
            OrderRefNum ref{};
            if (!(in >> ref)) {
                fmt::print(stderr, "line {}: CANCEL needs a reference\n", line_no);
                return false;
            }
            const Qty removed = book.cancel(ref);
            fmt::print(fmt::emphasis::bold, "\n> CANCEL ref {}\n", ref);
            if (removed > 0) {
                fmt::print(fmt::fg(fmt::color::orange), "  cancelled {} shares\n", removed);
                ++totals.cancelled;
            } else {
                fmt::print(fmt::fg(fmt::color::gray),
                           "  nothing to cancel — already filled, already cancelled, or never "
                           "existed\n");
            }
            print_book(book);
            continue;
        }

        // ---- BUY / SELL <qty> <price> ----
        const bool is_buy = (verb == "BUY" || verb == "buy");
        const bool is_sell = (verb == "SELL" || verb == "sell");
        if (!is_buy && !is_sell) {
            fmt::print(stderr, "line {}: unknown command '{}'\n", line_no, verb);
            return false;
        }

        Qty qty{};
        std::string price_text;
        if (!(in >> qty >> price_text)) {
            fmt::print(stderr, "line {}: {} needs a quantity and a price\n", line_no, verb);
            return false;
        }
        Price limit{};
        if (!Price::from_string(price_text, limit)) {
            fmt::print(stderr, "line {}: '{}' is not a price\n", line_no, price_text);
            return false;
        }

        OrderRequest req{};
        req.ref = ++next_ref;
        req.side = is_buy ? Side::Buy : Side::Sell;
        req.type = OrderType::Limit;
        req.tif = TimeInForce::Day;
        req.qty = qty;
        req.limit_price = limit;

        const Qty rested = book.submit(req, fills);
        ++totals.submitted;

        fmt::print(fmt::emphasis::bold, "\n> {} {} @ {}  (ref {})\n", is_buy ? "BUY " : "SELL", qty,
                   limit.to_string(), req.ref);

        for (const Fill& f : fills) {
            // Printed at the RESTING order's price — see the note on Fill::price in order_book.hpp.
            fmt::print(fmt::fg(fmt::color::lime_green), "  FILL {} @ {}  (ref {} takes ref {})\n",
                       f.qty, f.price.to_string(), f.aggressor, f.resting);
            ++totals.fills;
            totals.volume += f.qty;
        }

        if (rested > 0) {
            fmt::print("  rested {}\n", rested);
        } else if (fills.empty()) {
            // `submit` returns 0 for "filled completely" AND for "refused", and the caller cannot
            // tell them apart. With no fills to show, this is the refusal case — a price off the
            // tick grid or outside ArrayOrderBook's window. The ambiguity is a known limitation,
            // recorded in docs/BENCHMARK-orderbook-v2.md rather than hidden.
            fmt::print(fmt::fg(fmt::color::red),
                       "  nothing filled, nothing rested — price is off-grid or outside this "
                       "book's window\n");
        } else {
            fmt::print("  fully filled, nothing rested\n");
        }
        print_book(book);
    }

    fmt::print(fmt::emphasis::bold, "\n── summary ──\n");
    fmt::print("  {} orders submitted, {} cancelled\n", totals.submitted, totals.cancelled);
    fmt::print("  {} fills, {} shares traded\n", totals.fills, totals.volume);
    fmt::print("  {} orders left resting\n\n", book.order_count());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Replay a script of orders through hft::matching and print the book after each."};
    std::string script_path;
    std::string which = "array";
    app.add_option("script", script_path, "Order script to replay")->required();
    app.add_option("--book", which, "Which implementation: array (default), map, pool, or flat")
        ->check(CLI::IsMember({"array", "map", "pool", "flat"}));
    CLI11_PARSE(app, argc, argv);

    std::ifstream script(script_path);
    if (!script) {
        fmt::print(stderr, "cannot open '{}'\n", script_path);
        return 1;
    }

    bool ok = false;
    if (which == "map") {
        ok = run_script<OrderBook>(script, "OrderBook (std::map price levels)");
    } else if (which == "pool") {
        ok = run_script<PoolOrderBook>(script, "PoolOrderBook (pooled, intrusively linked orders)");
    } else if (which == "flat") {
        ok = run_script<FlatOrderBook>(script, "FlatOrderBook (+ open-addressed ref table)");
    } else {
        ok = run_script<ArrayOrderBook>(script, "ArrayOrderBook (tick-indexed array + bitmap)");
    }
    return ok ? 0 : 1;
}
