// libs/matching/include/hft/matching/pool_order_book.hpp
// Responsibility: the same price-time-priority book as ArrayOrderBook, but resting orders come from
// a pre-allocated pool and are linked together directly, instead of being allocated one at a time.
//
// ONE VARIABLE CHANGES AGAIN. Price levels are still a flat tick-indexed array, the occupancy
// bitmap is still there, the touch is still cached, the matching logic is the same. Only where
// resting orders live is different.
//
// ---------------------------------------------------------------------------
// WHAT THIS REMOVES, AND WHY THIS ONE
// ---------------------------------------------------------------------------
//
// This is not a guess about what to optimise next. It is the cost the previous two rounds of
// measurement pointed at, twice, from different directions:
//
//   - docs/BENCHMARK-orderbook-v1.md finding (2): the resting path costs the same at 10 levels and
//     at 1000. So its cost is not the price lookup. It is the two `malloc` calls: one for a
//     `std::list` node, one for an `unordered_map` node.
//   - docs/BENCHMARK-orderbook-v2.md finding (1): cancel still gets slower as the book gets deeper,
//     even though every step in it is O(1). The reason is that the nodes it follows are separate
//     heap objects spread over about 1.5 MB at depth 1000, so each hop is likely a cache miss.
//
// Both point at allocation. This version removes the first of the two allocations.
//
// ---------------------------------------------------------------------------
// THREE SEPARATE IDEAS: INTRUSIVE, POOLED, INDEXED
// ---------------------------------------------------------------------------
//
// 1. INTRUSIVE. `std::list<RestingOrder>` allocates a node that WRAPS your object:
//
//        [ prev | next | RestingOrder{ref, qty} ]     <- one malloc per order
//
//    An intrusive list puts `prev` and `next` inside the object itself, so there is no wrapper to
//    allocate. The word "intrusive" describes the cost: the container's links intrude into your
//    type, which now has to carry them. You lose a clean type and automatic lifetime management,
//    and you get the allocation back.
//
// 2. POOLED. All the objects come from one array allocated once, at construction. Nothing is
//    allocated or freed while the book runs. "Freeing" an order means putting it back on a free
//    list, and that free list is itself linked through the SAME `next` field. An order is only ever
//    on one of the two lists, so they can share the field.
//
// 3. INDEXED. The links are `std::uint32_t` positions in the pool, not pointers. Half the size of a
//    pointer, so more of the object fits in a cache line. It also means the pool has no absolute
//    addresses in it, which matters if it ever needs to live in shared memory or be written to a
//    file. Cheaper to decide now than to change later. `kNil` is the null value.
//
// The three are separable — you could have an intrusive list of heap objects, or a pool of
// non-intrusive nodes — but together they are what "no allocation on the hot path" means in
// practice, and that phrase is a hard requirement in this domain rather than an optimisation.
//
// ---------------------------------------------------------------------------
// WHAT THIS VERSION DOES NOT FIX
// ---------------------------------------------------------------------------
//
// `refMap` is still a `std::unordered_map`, so ONE malloc per resting order remains. That is
// deliberate: removing both at once would make the result unattributable, which is the mistake v2
// made by changing the price container and the touch lookup together. Removing it is a separate
// step, and it has a prerequisite question — whether OrderRefNum is dense enough to index an array
// directly — that this version does not have to answer.
//
// The pool is a FIXED SIZE. If an order arrives when the free list is empty, it is rejected, the
// same way a price outside the window is rejected: `submit` returns 0 and nothing rests. Same
// limitation, same ambiguity (0 still means "filled completely" OR "refused"), written down rather
// than hidden.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/array_order_book.hpp"  // window constants are shared
#include "hft/matching/order_book.hpp"        // OrderRequest, Fill

namespace hft::matching {

// The null position: "there is no order here". Defined out here, ahead of the structs that need it,
// so their default member initializers can use it.
//
// Chosen as the largest uint32 rather than 0, because 0 is a valid slot in the pool. A null value
// that is also a legal value causes bugs that only appear under load — and it would break `unlink`,
// which decides whether an order is at the head of a queue by asking whether `prev` is null.
inline constexpr std::uint32_t kNil = 0xFFFF'FFFFu;

// A resting order that carries its own links. One per live order. They are handed out from
// `PoolOrderBook::pool_` and returned to the free list on cancel or full fill.
//
// Laid out to be 32 bytes so two fit in a 64-byte cache line. The static_assert below checks that,
// because padding rules are easy to get wrong when counting by hand.
struct PooledOrder {
    common::OrderRefNum ref{};   // 8 — the participant-visible reference
    common::Qty remainQty{};     // 8 — unfilled quantity
    std::uint32_t prev{kNil};    // 4 — previous order at this level, or kNil
    std::uint32_t next{kNil};    // 4 — next order at this level; also the free-list link
    std::uint32_t level{};       // 4 — which price level it rests at; cancel needs this
    common::Side side{};         // 1 — which side; cancel needs this
                                 // 3 bytes padding
};

// A price level, as two pool positions instead of a std::list. `head` is the front of the queue
// (oldest order, first to trade); `tail` is where new orders attach. Both kNil when the level is
// empty.
struct PoolLevel {
    common::Qty total_qty{};
    std::uint32_t head{kNil};
    std::uint32_t tail{kNil};
};

class PoolOrderBook {
public:
    // ---- The pool -----------------------------------------------------------
    //
    // Sized for the deepest benchmark fixture (1000 levels per side x 10 orders per level x 2 sides
    // = 20'000) with room to spare, rounded up to a power of two. At 32 bytes per order that is
    // 32'768 x 32 = 1 MiB, which is bigger than L2. That is expected: the point is not that the
    // pool fits in cache, it is that it is CONTIGUOUS, so orders created around the same time end
    // up near each other in memory. Separate mallocs give you no such guarantee.
    static constexpr std::uint32_t kMaxOrders = 32'768;

    // The price window is the same as ArrayOrderBook's, and is shared rather than copied: two
    // copies of a constant are two things that can end up disagreeing.
    static constexpr std::size_t kLevels = ArrayOrderBook::kLevels;
    static constexpr std::int64_t kTickSize = ArrayOrderBook::kTickSize;
    static constexpr std::int64_t kBaseTicks = ArrayOrderBook::kBaseTicks;
    static constexpr std::int64_t kCeilTicks = ArrayOrderBook::kCeilTicks;
    static constexpr std::size_t kNone = kLevels;  // "no touch on this side"

    PoolOrderBook();

    // ---- The same interface as OrderBook and ArrayOrderBook -----------------
    // The contracts are identical; see order_book.hpp for the full descriptions. There are two
    // reasons an order can be refused here instead of one: a price off the tick grid or outside the
    // window, and an empty pool.

    common::Qty submit(const OrderRequest& req, std::vector<Fill>& fills);
    common::Qty cancel(common::OrderRefNum ref);

    std::optional<common::Price> best_bid() const noexcept;
    std::optional<common::Price> best_ask() const noexcept;
    common::Qty qty_at(common::Side side, common::Price price) const noexcept;

    bool empty() const noexcept;
    std::size_t order_count() const noexcept;

private:
    // ---- TODO(you): the free list ------------------------------------------
    //
    // Two operations, both O(1):
    //
    //   alloc()        take the slot at free_head_, move free_head_ to that slot's `next`, return
    //                  the slot. Return kNil when the pool is empty; the caller has to check.
    //   release(slot)  set that slot's `next` to free_head_, then set free_head_ to the slot.
    //
    // This is a stack, not a queue, and that is deliberate: the slot freed most recently is the one
    // most likely to still be in cache.
    //
    // The constructor has to build the initial free list across the whole pool (slot i's `next` is
    // i + 1, and the last one is kNil). That is the only loop over the pool that ever runs.
    std::uint32_t alloc() noexcept;
    void release(std::uint32_t slot) noexcept;

    // ---- TODO(you): linking into and out of a level -------------------------
    //
    // `push_back` attaches at the tail, because newer orders queue behind older ones. That is what
    // time priority means. `unlink` removes an order from anywhere in the queue, which is what
    // cancel needs, and is the reason the list is doubly linked at all.
    //
    // UNLINK IS THE PART THAT GOES WRONG. There are four cases, and it is easy to write only one:
    //
    //     the only order at the level     -> head and tail both become kNil
    //     at the head, others behind      -> head moves forward; the new head's prev becomes kNil
    //     at the tail, others in front    -> tail moves back; the new tail's next becomes kNil
    //     in the middle                   -> the two neighbours point at each other
    //
    // Most people write the middle case first, because it is the one you picture. The other three
    // are the ones that corrupt the book later. Write all four out; `std::list` was handling them
    // for you before.
    void push_back(PoolLevel& lvl, std::uint32_t slot) noexcept;
    void unlink(PoolLevel& lvl, std::uint32_t slot) noexcept;

    // ---- Carried over from ArrayOrderBook, same shape -----------------------
    //
    // The bitmap and the cached touch are not being changed here. They were measured in v2.1 and
    // they work. Keep the same two helpers that own every bitmap update, for the same reason as
    // before: six scattered call sites is where the buy/sell mix-ups came from.
    void on_level_occupied(common::Side side, std::size_t idx) noexcept;
    void on_level_emptied(common::Side side, std::size_t idx) noexcept;

    static bool index(common::Price price, std::size_t& out) noexcept;

    // ---- State --------------------------------------------------------------
    std::array<PooledOrder, kMaxOrders> pool_{};
    std::uint32_t free_head_{kNil};

    std::array<PoolLevel, kLevels> buy_levels_{};
    std::array<PoolLevel, kLevels> sell_levels_{};
    std::uint64_t buy_occupied_[kLevels / 64]{};
    std::uint64_t sell_occupied_[kLevels / 64]{};

    std::size_t best_bid_idx_{kNone};
    std::size_t best_ask_idx_{kNone};

    std::size_t buy_orders_count{};
    std::size_t sell_orders_count{};

    // Still one malloc per resting order. See "WHAT THIS VERSION DOES NOT FIX" above. The value is
    // now just a pool position, not a locator struct, because a PooledOrder already knows its own
    // side and level.
    std::unordered_map<common::OrderRefNum, std::uint32_t> refMap;
};

// Check the layout instead of trusting the byte counts in the comment above.
static_assert(sizeof(PooledOrder) == 32, "PooledOrder should stay at 32 bytes; two per cache line");
static_assert(PoolOrderBook::kMaxOrders < kNil, "kNil must not be a valid slot");

}  // namespace hft::matching
