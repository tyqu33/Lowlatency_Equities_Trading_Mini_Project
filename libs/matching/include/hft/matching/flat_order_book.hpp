// libs/matching/include/hft/matching/flat_order_book.hpp
// Responsibility: the same price-time-priority book as PoolOrderBook, with the last heap-allocating
// container removed. Every structure in this book is now a flat array, which is what "flat" means
// in the name.
//
// ONE VARIABLE CHANGES AGAIN. Price levels are still a tick-indexed array, the occupancy bitmap and
// cached touch are unchanged, resting orders still come from the same pooled intrusive list. Only
// the ref index differs: `std::unordered_map<OrderRefNum, uint32_t>` becomes an open-addressed hash
// table living in a fixed array.
//
// ---------------------------------------------------------------------------
// WHY THIS ONE, AND WHY IT IS THE LAST
// ---------------------------------------------------------------------------
//
// Three rounds of measurement have now pointed at this structure, and the third pointed at it by
// failing to move:
//
//   - v1 finding (2): the resting path is dominated by two mallocs, not by the price lookup.
//   - v2 finding (1): cancel's depth curve is not a complexity effect. Every step is O(1); the
//     growth is the cost of chasing pointers across a working set that grows with depth.
//   - v3 finding (1): pooling the ORDER nodes dropped cancel's whole curve by ~30 ns and left its
//     slope unchanged to within 0.01 ns (+19.53 ns vs +19.52 ns from depth 10 to 1000). Removing
//     one scattered structure did not change the slope, which says the slope belongs to the other
//     one — the ~1.5 MB of `unordered_map` nodes that malloc placed wherever it liked.
//
// After this there is no allocation left on either hot path, and the claim "allocation-free hot
// path" becomes true rather than nearly true.
//
// ---------------------------------------------------------------------------
// WHY A HASH TABLE AND NOT A DIRECT ARRAY
// ---------------------------------------------------------------------------
//
// The requirement driving this version is the one this project set for itself: no allocation on
// the hot path. `std::unordered_map` fails it for a specific reason — separate chaining puts each
// key in its own heap node — and any replacement has to hold every entry in storage reserved up
// front. Both a direct array and an open-addressed table do that. The choice between them is a
// separate question.
//
// A DIRECT ARRAY, `slot_of[ref]`, is the faster of the two and is the right answer when the
// reference numbers are dense over a bounded range. Nothing here rules that out: this venue issues
// its own OrderRefNums, so it could define them to be dense and index them directly. Doing that
// would mean committing to two things — that the numbering scheme never develops gaps, and that
// the live range never outgrows the array — and enforcing both at the point where numbers are
// issued, which is outside this class.
//
// This version does not take that route, for one reason: it would make the book's memory safety
// depend on a property of the identifier stream that nothing in the book can check. An
// out-of-range or unexpectedly sparse ref is an out-of-bounds access, not a wrong answer. An
// open-addressed table costs a probe and is correct for any distribution, which is worth more here
// than the difference between one load and one-point-something loads.
//
// That is a judgement about this design, not a general rule. A venue that does control and
// guarantee its numbering can reasonably index directly, and would be faster for it.
//
// ---------------------------------------------------------------------------
// WHAT AN OPEN-ADDRESSED TABLE COSTS
// ---------------------------------------------------------------------------
//
// `std::unordered_map` is separate chaining: each key lives in its own heap node, hanging off a
// bucket. That is what allocates, and what scatters. Open addressing puts every entry in one flat
// array and resolves collisions by walking to the next slot.
//
// The cost is that DELETION IS NO LONGER SIMPLE. Clearing an entry would break every probe
// sequence that ran through it: a later key that had to walk past this slot to reach its home would
// now find an empty slot first and conclude the key is absent. Two standard fixes:
//
//   - TOMBSTONES: mark the slot deleted-but-not-empty. Probes walk past tombstones; insertions may
//     reuse them. Simple and constant time, but tombstones accumulate and slowly lengthen probes
//     until something rehashes the table.
//   - BACKWARD-SHIFT DELETION: move the following cluster back to close the gap. No accumulation,
//     several times more code, and each delete touches more memory.
//
// This version uses TOMBSTONES, chosen deliberately as the simpler thing first, with the intent of
// measuring what they cost before deciding whether they need fixing. That decision has a
// measurement consequence worth writing down in advance, because it is a hole in the current
// benchmark rather than a result:
//
//     TOMBSTONE ACCUMULATION IS A FUNCTION OF SESSION LENGTH, NOT OF BOOK DEPTH. The benchmark
//     rebuilds its fixture whenever it runs out of prepared work, which also resets this table.
//     A cost that only appears after millions of cancels against one long-lived book is one this
//     harness is currently shaped not to see.
//
// `std::unordered_map` has no equivalent problem, because erasing a chained node just unlinks it.
// This is the same kind of trade the intrusive list made: the container stops managing something,
// and the code has to.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/array_order_book.hpp"  // window constants are shared
#include "hft/matching/order_book.hpp"        // OrderRequest, Fill
#include "hft/matching/pool_order_book.hpp"   // kNil, PooledOrder, PoolLevel

namespace hft::matching {

// One entry in the ref table. `state` is explicit rather than encoded into a reserved value of
// `ref` or `slot`, because a sentinel that is also a legal value is the bug that only shows up
// under load — the same reason kNil is 0xFFFFFFFF and not 0.
enum class RefEntryState : std::uint8_t {
    Empty,      // never used; a probe that reaches one of these can stop
    Occupied,   // holds a live ref -> slot mapping
    Tombstone,  // was occupied; a probe must walk PAST it, an insert may reuse it
};

struct RefEntry {
    common::OrderRefNum ref{};                       // 8
    std::uint32_t slot{};                            // 4
    RefEntryState state{RefEntryState::Empty};       // 1 (+3 padding)
};

class FlatOrderBook {
public:
    // ---- The pool, unchanged from v3 ----------------------------------------
    static constexpr std::uint32_t kMaxOrders = PoolOrderBook::kMaxOrders;  // 32768

    // ---- The ref table ------------------------------------------------------
    //
    // A power of two so the home slot is `ref & kRefMask` — a bitwise AND instead of a division.
    // Twice kMaxOrders, so the load factor never exceeds 0.5 even with every order resting at once.
    // Above roughly that point, linear probing's average probe length starts climbing sharply; below
    // it, it stays near one.
    //
    // 65'536 x 16 bytes = 1 MiB, on top of the pool's 1 MiB. That is not small, and it is the price
    // of never allocating: the memory is reserved up front instead of being taken and returned a
    // node at a time.
    static constexpr std::size_t kRefTableSize = 65'536;
    static constexpr std::size_t kRefMask = kRefTableSize - 1;

    // ---- The price window, shared with the other two books ------------------
    static constexpr std::size_t kLevels = ArrayOrderBook::kLevels;
    static constexpr std::int64_t kTickSize = ArrayOrderBook::kTickSize;
    static constexpr std::int64_t kBaseTicks = ArrayOrderBook::kBaseTicks;
    static constexpr std::int64_t kCeilTicks = ArrayOrderBook::kCeilTicks;
    static constexpr std::size_t kNone = kLevels;

    FlatOrderBook();

    // ---- The same interface as the other three books ------------------------
    // Identical contracts; see order_book.hpp. Three ways to refuse an order rather than two: a
    // price off-grid or outside the window, an exhausted pool, and now a full ref table.

    common::Qty submit(const OrderRequest& req, std::vector<Fill>& fills);
    common::Qty cancel(common::OrderRefNum ref);

    std::optional<common::Price> best_bid() const noexcept;
    std::optional<common::Price> best_ask() const noexcept;
    common::Qty qty_at(common::Side side, common::Price price) const noexcept;

    bool empty() const noexcept;
    std::size_t order_count() const noexcept;

private:
    // ---- TODO(you): the ref table -------------------------------------------
    //
    // Three operations. All three walk the same probe sequence, and the walk is the same three
    // lines each time; what differs is what each one does with the states it meets.
    //
    //   home slot     h = ref & kRefMask
    //   next slot     h = (h + 1) & kRefMask        <- wraps without a branch
    //
    // ON THE HASH FUNCTION. `ref & kRefMask` is the identity, and here that is not laziness, it is
    // the best available choice. This venue hands out reference numbers from a counter, so
    // consecutive orders get consecutive home slots: no collisions at all in the common case, and
    // a probe that does collide walks forward into memory the previous probe already pulled into
    // cache. A scrambling hash would take that ordering — which the design went to the trouble of
    // producing — and destroy it. The usual advice to avoid identity hashes exists because keys are
    // usually not under your control; here they are.
    //
    //   ref_find(ref)    -> index of the Occupied entry holding `ref`, or kRefTableSize if absent.
    //                       Walk while the state is not Empty. Stop and report absent at Empty;
    //                       KEEP GOING past Tombstone. Getting that one wrong makes cancel silently
    //                       fail for any order whose probe crossed a deleted slot, which is
    //                       depth-dependent and load-dependent and very hard to reproduce.
    //   ref_insert(ref, slot) -> true on success. Walk to the first Empty or Tombstone and take it.
    //                       Return false if the table is full (see the note on the guard below).
    //   ref_erase(ref)   -> mark the entry Tombstone. Not Empty; see the header notes.
    //
    // THE FULL-TABLE GUARD. A probe loop that only stops at Empty runs forever on a full table.
    // With the load factor capped at 0.5 this cannot happen, which is exactly the argument that
    // makes people leave the guard out — and then it is an infinite loop the first time an
    // invariant slips. Bound the walk by kRefTableSize steps.
    std::size_t ref_find(common::OrderRefNum ref) const noexcept;
    bool ref_insert(common::OrderRefNum ref, std::uint32_t slot) noexcept;
    void ref_erase(common::OrderRefNum ref) noexcept;

    // ---- Carried over from PoolOrderBook, unchanged --------------------------
    // These are the same functions you already wrote and the tests already verified. They are
    // repeated rather than shared so each book reads on its own; if a fourth copy appears, that is
    // the point to hoist them.
    std::uint32_t alloc() noexcept;
    void release(std::uint32_t slot) noexcept;
    void push_back(PoolLevel& lvl, std::uint32_t slot) noexcept;
    void unlink(PoolLevel& lvl, std::uint32_t slot) noexcept;
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

    // The last heap container, gone. Nothing in this class allocates after construction.
    std::array<RefEntry, kRefTableSize> ref_table_{};
    std::size_t ref_live_{};       // Occupied entries
    std::size_t ref_tombstones_{}; // Tombstone entries — the number to watch
};

static_assert(sizeof(RefEntry) == 16, "RefEntry should stay at 16 bytes; four per cache line");
static_assert((FlatOrderBook::kRefTableSize & FlatOrderBook::kRefMask) == 0,
              "kRefTableSize must be a power of two for the mask to work");
static_assert(FlatOrderBook::kRefTableSize >= 2 * FlatOrderBook::kMaxOrders,
              "load factor must stay at or below 0.5");

}  // namespace hft::matching
