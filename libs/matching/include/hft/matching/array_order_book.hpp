// libs/matching/include/hft/matching/array_order_book.hpp
// Responsibility: the same price-time-priority book as OrderBook, with the ordered map of price
// levels replaced by a flat array indexed directly by price.
//
// ONE VARIABLE CHANGES. The FIFO queue inside each level is still std::list; the ref index is still
// std::unordered_map; the matching logic is the same walk outward from the touch. Only the
// price -> level lookup differs. That is deliberate: the baseline in docs/BENCHMARK-orderbook-v1.md
// makes three predictions about what this change will and will not improve, and swapping several
// things at once would make the result unattributable.
//
// The interface is identical to OrderBook's, so tests/test_order_book.cpp runs the same suite
// against both and proves they behave the same.
//
// ---------------------------------------------------------------------------
// WHAT AN ARRAY BUYS, AND WHAT IT COSTS
// ---------------------------------------------------------------------------
//
// Prices are integers on a fixed grid, so `index = (price - base) / tick_size` reaches a level in
// one arithmetic step and one load — no tree descent, no pointer chasing between nodes scattered
// across the heap. The baseline measured cancel at 65.8 / 95.0 / 135.4 ns across depths 10 / 100 /
// 1000; that curve is a red-black tree's log(n) plus, at the deepest end, a working set that no
// longer fits in L1.
//
// The cost is that an array has to be allocated over a FIXED PRICE RANGE, and anything outside it
// has nowhere to go. A real venue rebases the window as the market moves. This version does the
// simplest thing instead:
//
//     PRICES OUTSIDE THE WINDOW, OR NOT ON THE TICK GRID, ARE REJECTED.
//     `submit` returns 0 and nothing rests; `cancel` and the read accessors treat them as absent.
//
// That is a real limitation, stated rather than hidden. Note the ambiguity it introduces: a return
// of 0 from `submit` now means "filled completely" OR "refused" — the caller cannot tell them
// apart. Worth fixing before this book is wired to anything that has to answer a participant.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/order_book.hpp"  // OrderRequest, Fill, RestingOrder, Level

namespace hft::matching {

struct ArrayLocator{ 
    common::Side side;
    std::size_t index;
    std::list<RestingOrder>::iterator it;
};

class ArrayOrderBook {
public:
    // ---- The price window --------------------------------------------------
    //
    // Sized so the default covers every price the test suite and the benchmark use, with room to
    // spare. A power of two makes the bitmap arithmetic below tidy.
    //
    //   levels          4096
    //   tick size       100 internal ticks = $0.01, the Rule 612 increment above $1.00
    //   index 2048      $100.0000, the reference price both the tests and the benchmark centre on
    //   covered range   $79.52 ... $120.47
    //
    // Indexing by GRID STEP rather than by raw tick matters: at 1/10000 USD per tick a $40 window
    // would need 400'000 entries, and the cache behaviour that motivates this whole exercise would
    // be lost to the 99% of entries that can never be occupied.
    static constexpr std::size_t kLevels = 4096;
    static constexpr std::int64_t kTickSize = 100;
    static constexpr std::size_t kCenterIndex = kLevels / 2;
    static constexpr std::int64_t kBaseTicks =
        1'000'000 - static_cast<std::int64_t>(kCenterIndex) * kTickSize;
    static constexpr std::int64_t kCeilTicks =
        kBaseTicks + (kLevels - 1) * kTickSize;

    ArrayOrderBook() = default;

    // ---- The same interface as OrderBook ------------------------------------
    // Contracts are identical; see order_book.hpp for the full descriptions. The only difference
    // is the rejection of prices outside the window, described at the top of this file.

    common::Qty submit(const OrderRequest& req, std::vector<Fill>& fills);
    common::Qty cancel(common::OrderRefNum ref);

    std::optional<common::Price> best_bid() const noexcept;
    std::optional<common::Price> best_ask() const noexcept;
    common::Qty qty_at(common::Side side, common::Price price) const noexcept;

    bool empty() const noexcept;
    std::size_t order_count() const noexcept;

private:
    // TODO(you): the data structures.
    //
    // Two of the three carry over from OrderBook unchanged — a std::list of RestingOrder per level
    // for time priority, and an unordered_map from OrderRefNum to a locator for O(1) cancel. Only
    // the first one changes shape:
    //
    //   1. price -> level.  An array of kLevels entries per side. `Level` is already defined in
    //      order_book.hpp and can be reused as-is. Nothing needs erasing when a level empties: an
    //      empty level is just one whose list is empty, which is one fewer container operation than
    //      the map version had to perform.
    //
    //   2. FINDING THE BEST PRICE is the part with no direct equivalent. `std::map` handed it to
    //      you as `begin()`; an array has to be searched. Scanning outward from the last known best
    //      works and is easy to get right, but degrades exactly where the map did — when the book
    //      is deep and sparse.
    //
    //      The alternative is a BITMAP: one bit per level, set when the level becomes occupied and
    //      cleared when it empties. kLevels bits is kLevels/64 words of std::uint64_t. Finding the
    //      lowest occupied level is then a scan over those words plus `std::countr_zero` (<bit>,
    //      C++20) on the first non-zero one — a single instruction that returns the index of the
    //      lowest set bit. 4096 levels is 64 words, so the worst case is 64 loads and one
    //      countr_zero, and the common case is one load.
    //
    //      For the bid side you want the HIGHEST occupied level, so you want the highest set bit:
    //      `std::countl_zero` counts leading zeros, and 63 - countl_zero gives you the index within
    //      a word. Scanning the words from the top end rather than the bottom.
    //
    //   3. Index arithmetic. `(price.ticks() - kBaseTicks) / kTickSize`, valid only when the price
    //      is on the grid (the division has no remainder) and the index lands in [0, kLevels).
    //      Write one helper that returns both facts and use it everywhere — the three read
    //      accessors, submit, and cancel all need the same check, and duplicating it is how the
    //      window bound and the grid check end up disagreeing.
    //
    // Keep the running order counters from OrderBook: with no map to consult, `empty()` and
    // `order_count()` have nothing else to ask.
    std::size_t buy_orders_count{};   // 订单笔数
    std::size_t sell_orders_count{};  // 订单笔数
    std::uint64_t buy_occupied_[kLevels / 64]{};   // bitmap, one entry per Level, 4096 bits = 512 bytes = 8 cache lines
    std::uint64_t sell_occupied_[kLevels / 64]{};
    std::array<Level, kLevels> buy_levels_{};      // array of Levels, every entry is a Level(total_qry + list), 4096 x 32 bytes = 128 KB
    std::array<Level, kLevels> sell_levels_{};
    std::unordered_map<common::OrderRefNum, ArrayLocator> refMap;

    static bool index(common::Price price, std::size_t& out) noexcept;
};

}  // namespace hft::matching
