// libs/matching/include/hft/matching/order_book.hpp
// Responsibility: a single-symbol, price-time-priority limit order book — the matching core.
//
// This is a LIBRARY, not part of an executable, for one reason: it is the piece that most needs
// unit tests and benchmarks, and neither can link against code buried in an `add_executable`
// target. `apps/venue/me` is the process shell that owns one book per symbol, speaks OUCH in and
// ITCH out, and does no matching of its own.
//
// SCOPE OF v1: one symbol, limit orders, day orders. Everything below is shaped so that market
// orders, IOC/FOK, and multi-symbol slot in later without changing this interface.
//
// WHAT THIS CLASS DOES NOT DO:
//   - Assign identifiers. The venue does that; `OrderRefNum` arrives already assigned.
//   - Know about symbols. One instance == one symbol. The caller holds symbol -> book.
//   - Read a clock. Time priority is arrival ORDER, tracked by an internal counter. A wall-clock
//     timestamp would be non-deterministic (breaking the replay guarantee ADR-0005 depends on) and
//     can tie between two orders in the same microsecond; a counter can do neither.
//   - Allocate on the hot path, once warmed up. See the note on `submit` below.

#pragma once

#include <cstddef>
#include <optional>
#include <vector>
#include <map>
#include <list>
#include <unordered_map>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"

namespace hft::matching {

// ============================================================================
// Value types
// ============================================================================

// An order as submitted to the book.
struct OrderRequest {
    common::OrderRefNum ref{};   // venue-assigned; unique among live orders in this book
    common::Side side{};         //
    common::OrderType type{};    //
    common::TimeInForce tif{};   // v1 only needs Day; IOC/FOK change whether the remainder rests
    common::Qty qty{};           // must be > 0

    // The sender's price LIMIT — and only that.
    //
    // Meaningful only when `type == OrderType::Limit`. For a market order this field is not a cap
    // on the execution price; treating it as one is the bug this naming exists to prevent. (A
    // broker that stuffs a reference price here for margin purposes — standard practice in some
    // markets — makes the field look populated and trustworthy when it is neither.) Gate every read
    // of it on `type`.
    common::Price limit_price{};
};

// One execution, produced when an incoming order matches a resting one.
struct Fill {
    common::OrderRefNum aggressor{};  // the incoming order that crossed the spread
    common::OrderRefNum resting{};    // the order that was already sitting in the book

    // Executions happen at the RESTING order's price, not the aggressor's. If a buy limit at 101
    // hits a resting sell at 100, the trade prints at 100 and the buyer keeps the 1-tick
    // improvement — the resting order set the terms by being there first, which is the entire
    // economic point of posting.
    common::Price price{};
    common::Qty qty{};
};

struct RestingOrder{
    common::OrderRefNum resting{};
    common::Qty remainQty{};
};

struct Level{
    common::Qty total_qty{};
    std::list<RestingOrder> orders;
};

struct Locator{
    common::Side side;
    common::Price price;
    std::list<RestingOrder>::iterator it; // points to buy's/sell's Level's orders
};

// ============================================================================
// OrderBook
// ============================================================================

class OrderBook {
private:
    // Three structures, each solving a different lookup:
    //
    //   1. price -> level, ordered.  Matching always walks from the best price outward, so this
    //      must keep prices sorted. Bids descend, asks ascend — two containers, or one with an
    //      inverted comparator.
    //
    //   2. within a level: orders in arrival order.  Time priority means first in, first filled,
    //      and matching consumes from the front while cancels remove from the middle. That shape
    //      argues for a list rather than a vector.
    //
    //   3. ref -> where the order lives.  `cancel` receives only an OrderRefNum. Without this,
    //      cancelling means scanning every level — and cancels outnumber fills heavily in real
    //      flow, so this is the difference between O(1) and O(n) on the most common operation.
    //
    // (1) is a `std::map` for now — correct and obvious, and the baseline the tick-indexed array
    // will be benchmarked against. The bid side carries a reversed comparator so that `begin()` is
    // the best price on BOTH sides, which keeps the two matching loops the same shape.
    std::size_t buy_orders_count{}; // 订单笔数
    std::size_t sell_orders_count{};// 订单笔数
    std::map<common::Price, Level, std::greater<common::Price>> buy; // bid - 买价从高到低
    std::map<common::Price, Level> sell; // ask - 卖价从低到高
    // std::list<common::OrderRefNum, OrderRequest> q;
    std::unordered_map<common::OrderRefNum, Locator> refMap;


public:
    OrderBook() = default;

    // Submit an order: match it against the opposite side while it can trade, then rest whatever
    // is left (if the order type and TIF allow resting).
    //
    // Fills are appended to `fills`, which is CLEARED first. The caller owns and reuses that
    // vector, so after a few calls its capacity stops growing and the hot path is allocation-free —
    // the benefit of returning a `std::vector<Fill>` by value would be one allocation per order,
    // forever. Reusing a caller-owned buffer is the standard shape for this.
    //
    // Returns the quantity left resting in the book: 0 means fully filled (or cancelled without
    // resting), which is what tells the caller whether to publish an Add Order on the feed.
    common::Qty submit(const OrderRequest& req, std::vector<Fill>& fills);

    // Remove a resting order, returning the quantity that was actually cancelled.
    //
    // That quantity is what the venue has to report — OUCH's Canceled and ITCH's Order Cancel both
    // carry a share count — which a bool cannot supply. Note that no bookkeeping of the original
    // order size is needed for this: the book stores REMAINING quantity, so an order that partly
    // filled before the cancel arrived simply returns whatever was still resting.
    //
    // Returns 0 when `ref` is not in the book. That is not an error — the order may have filled or
    // been cancelled while the request was in flight, and a real venue answers with a Cancel Reject
    // rather than a crash. 0 is unambiguous as a "not found" signal because a resting order can
    // never have zero remaining; it would have left the book already.
    common::Qty cancel(common::OrderRefNum ref);

    // ---- Read-only views (for the market-data publisher and for tests) -----

    // Best bid / best ask. Empty when that side has no resting orders.
    std::optional<common::Price> best_bid() const noexcept; // bid - buy
    std::optional<common::Price> best_ask() const noexcept; // ask - sell

    // Total resting quantity at one price level. 0 if the level does not exist.
    common::Qty qty_at(common::Side side, common::Price price) const noexcept;

    bool empty() const noexcept;
    std::size_t order_count() const noexcept;


};
}  // namespace hft::matching
