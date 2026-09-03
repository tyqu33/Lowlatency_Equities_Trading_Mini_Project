// libs/matching/src/flat_order_book.cpp
// Responsibility: matching and book maintenance with no heap allocation anywhere on the hot path.
//
// Everything except the ref table is carried over from PoolOrderBook unchanged — the same code the
// shared test suite already verified. The only TODO(you) in this file is the open-addressed table
// and nothing else, which is the point: one variable changes, so the benchmark delta is
// attributable to it.
//
// Suggested order:
//
//   1. ref_find                (nothing works yet; every submit still refuses)
//   2. ref_insert              (Resting / Levels / Matching cases go green)
//   3. ref_erase               (Cancel cases go green)
//
// Step 1 has no test feedback on its own, because a table nothing was ever inserted into answers
// "absent" correctly by accident. The first real signal arrives at step 2, and if the probe loop in
// step 1 is wrong that is where it shows up.

#include "hft/matching/flat_order_book.hpp"

#include <bit>

namespace hft::matching {
namespace {
    constexpr std::size_t kWords = ArrayOrderBook::kLevels / 64;

    // 返回 >= from 的第一个置位档位;没有则返回 kLevels(哨兵)
    std::size_t next_up(const std::uint64_t* bm, std::size_t from) noexcept {
        if(from >= ArrayOrderBook::kLevels) return ArrayOrderBook::kLevels;
        std::size_t w = from / 64;
        std::uint64_t word = bm[w] & (~0ULL << (from % 64)); // 买单向上走
        while(true){
            if(word != 0) return w * 64 + static_cast<std::size_t>(std::countr_zero(word));
            if(++w == kWords) return ArrayOrderBook::kLevels;
            word = bm[w];
        }
    }
    // 返回 <= from 的第一个置位档位;没有则返回 kLevels(哨兵)
    std::size_t next_down(const std::uint64_t* bm, std::size_t from) noexcept {
        if(from >= ArrayOrderBook::kLevels) return ArrayOrderBook::kLevels;
        std::size_t w = from / 64;
        std::uint64_t word = bm[w] & (~0ULL >> (63 - from % 64)); // 卖单向下走, 63 - k 落在 [0, 63]
        while(true){
            if(word != 0) return w * 64 + static_cast<std::size_t>(63 - std::countl_zero(word));
            if(w == 0) return ArrayOrderBook::kLevels;
            else --w;
            word = bm[w];
        }
    }
}  // namespace

// ---------------------------------------------------------------------------
// TODO(you): the ref table. This is the only new code in this version.
// ---------------------------------------------------------------------------
//
// All three walk the same probe sequence:
//
//     std::size_t h = static_cast<std::size_t>(ref) & kRefMask;   // home slot
//     ...
//     h = (h + 1) & kRefMask;                                     // next slot, wraps
//
// What differs is how each one reacts to the three states it can land on. Write the state machine
// out per function rather than trying to share it; they genuinely differ, and the difference is
// where the bugs are.
//
// Bound every loop by kRefTableSize steps. At a load factor of 0.5 a full table cannot happen,
// which is the argument for leaving the bound out, and the reason its absence is an infinite loop
// rather than a wrong answer the day that stops being true.

// Index of the Occupied entry holding `ref`, or kRefTableSize when absent.
//
//   Empty      -> the key cannot be further along. Report absent.
//   Occupied   -> compare the stored ref. Match means found; otherwise keep walking.
//   Tombstone  -> KEEP WALKING. This is the one that matters: a tombstone means something used to
//                 live here, so a key whose probe passed through this slot may still be ahead. Stop
//                 here and cancel silently fails for that order.
std::size_t FlatOrderBook::ref_find(common::OrderRefNum ref) const noexcept {
    std::size_t h = static_cast<std::size_t>(ref) & kRefMask;
    for (std::size_t n = 0; n < kRefTableSize; ++n){ // 保证扫描从h开始，h = (h + 1) & kRefMask能绕回, n只用来兜底,不参与寻址
        const RefEntry& e = ref_table_[h];
        if(e.state == RefEntryState::Empty) return kRefTableSize;
        if(e.state == RefEntryState::Occupied && e.ref == ref) return h;
        h = (h + 1) & kRefMask;
    }
    return kRefTableSize;
}

// Store ref -> slot. Return false only when the table is full.
//
// Walk to the first Empty or Tombstone and take it; both are free space as far as an insert is
// concerned. Update ref_live_, and decrement ref_tombstones_ when a tombstone is reused — that
// counter is the one that says whether tombstones are a problem worth fixing.
//
// Callers guarantee the ref is not already present (submit only inserts refs it has just been
// handed, and a duplicate live ref is a caller bug rather than something to paper over here).
bool FlatOrderBook::ref_insert(common::OrderRefNum ref, std::uint32_t slot) noexcept {
    std::size_t h = static_cast<std::size_t>(ref) & kRefMask;
    std::size_t n = 0;
    for (; n < kRefTableSize; ++n){
        const RefEntry& e = ref_table_[h];
        if(e.state == RefEntryState::Empty){
            ref_live_++;
            break;
        } else if(e.state == RefEntryState::Tombstone){ // Callers guarantee the ref is not already present
            ref_live_++;
            ref_tombstones_--;
            break;
        }
        h = (h + 1) & kRefMask;
    }
    if(n == kRefTableSize) return false;
    ref_table_[h] = {ref, slot, RefEntryState::Occupied};
    return true;
}

// Mark the entry for `ref` as Tombstone. Absent is not an error; cancel already handled
// that case before calling.
void FlatOrderBook::ref_erase(common::OrderRefNum ref) noexcept {
    std::size_t h = static_cast<std::size_t>(ref) & kRefMask;
    for (std::size_t n = 0; n < kRefTableSize; ++n){
        RefEntry& e = ref_table_[h];
        if(e.state == RefEntryState::Empty) break;
        else if(e.state == RefEntryState::Occupied && e.ref == ref){
            e.state = RefEntryState::Tombstone;
            ref_tombstones_++; ref_live_--;
            break;
        }
        h = (h + 1) & kRefMask;
    }
}

// Carried over from PoolOrderBook: build the free list across the whole pool.
// The ref table needs no setup — RefEntry default-initialises to Empty, which is what an unused
// slot means, so `ref_table_{}` in the header has already done the work.
//
// Slot i's `next` is i + 1, the last slot's `next` is kNil, and free_head_ starts at 0. This is the
// only loop over the pool that ever runs; after this, allocating an order is two assignments.
FlatOrderBook::FlatOrderBook() {
    for(std::uint32_t i=0; i + 1 < kMaxOrders; i++){
        pool_[i].next = i+1;
    }
    pool_[kMaxOrders - 1].next = kNil;
    free_head_ = 0;
}

// Take a slot off the free list, or return kNil if there are none left.
std::uint32_t FlatOrderBook::alloc() noexcept {
    if(free_head_ == kNil) return kNil;
    const std::uint32_t slot = free_head_;
    free_head_ = pool_[slot].next;
    return slot;
}

// Put a slot back on the free list.
void FlatOrderBook::release(std::uint32_t slot) noexcept {
    pool_[slot].next = free_head_;
    free_head_ = slot;
}

// Attach `slot` at the tail of `lvl`. Newest goes last — that is time priority.
//
// Two cases: the level is empty (head and tail both become slot), or it is not (the old tail's
// `next` and the new order's `prev` point at each other, then tail moves).
void FlatOrderBook::push_back(PoolLevel& lvl, std::uint32_t slot) noexcept {
    pool_[slot].next = kNil;
    if(lvl.head == kNil){
        pool_[slot].prev = kNil;
        lvl.head = slot;
        lvl.tail = slot;
    } else {
        pool_[slot].prev = lvl.tail;
        pool_[lvl.tail].next = slot;
        lvl.tail = slot;        
    }
    lvl.total_qty += pool_[slot].remainQty;
}

// Remove `slot` from `lvl`, wherever it sits. FOUR cases — see the header.
//
// Work from the two neighbours. If `prev` is kNil this order was the head, so head moves forward;
// otherwise the previous order's `next` skips over it. Same idea on the other side for `next` and
// tail. Do not assume the order is in the middle.
void FlatOrderBook::unlink(PoolLevel& lvl, std::uint32_t slot) noexcept {
    if(pool_[slot].prev == kNil && pool_[slot].next == kNil){ // the only order at the level
        lvl.head = kNil;
        lvl.tail = kNil;
    } else if(pool_[slot].prev == kNil){ // at the head, others behind
        std::uint32_t nxt_slot = pool_[slot].next;
        lvl.head = nxt_slot;
        pool_[nxt_slot].prev = kNil;
    } else if(pool_[slot].next == kNil){ // at the tail, others in front
        std::uint32_t prv_slot = pool_[slot].prev;
        lvl.tail = prv_slot;
        pool_[prv_slot].next = kNil;
    } else { // in the middle
        std::uint32_t prv_slot = pool_[slot].prev;
        std::uint32_t nxt_slot = pool_[slot].next;
        pool_[prv_slot].next = nxt_slot;
        pool_[nxt_slot].prev = prv_slot;
    }
    lvl.total_qty -= pool_[slot].remainQty;
}

// Price -> level index. Returns false when the price is off-grid or outside the window.
//
// Same as ArrayOrderBook::index. Copied rather than shared so the two books can be read on their
// own. If a third copy ever shows up, that is the point to move it somewhere common.
bool FlatOrderBook::index(common::Price price, std::size_t& out) noexcept {
    std::int64_t price_tick = price.ticks();
    if(price_tick < FlatOrderBook::kBaseTicks || price_tick > FlatOrderBook::kCeilTicks) return false;
    if((price_tick - kBaseTicks) % kTickSize != 0) return false;
    out = static_cast<std::size_t>(price_tick - FlatOrderBook::kBaseTicks) / FlatOrderBook::kTickSize;
    return true;
}

// Set the occupancy bit, and move the touch out if this level is now the best.
void FlatOrderBook::on_level_occupied(common::Side side, std::size_t idx) noexcept {
    if(side == common::Side::Buy){
        buy_occupied_[idx/64] |= (1ULL << (idx % 64));
        if(best_bid_idx_ == kNone || idx > best_bid_idx_) best_bid_idx_ = idx;   // 买盘:越高越好
    } else {
        sell_occupied_[idx/64] |= (1ULL << (idx % 64));
        if(best_ask_idx_ == kNone || idx < best_ask_idx_) best_ask_idx_ = idx;   // 卖盘:越低越好
    }
}

// TODO(you): clear the occupancy bit, and if this level WAS the touch, find the next one.
//
// The bitmap search only runs here, on the narrowing branch. That was the whole point of v2.1 and
// it carries over unchanged. `kNone == kLevels` is still the same value the bitmap helpers return
// when they find nothing, so an empty book needs no special case.
void FlatOrderBook::on_level_emptied(common::Side side, std::size_t idx) noexcept {
    if(side == common::Side::Buy){
        buy_occupied_[idx/64] &= ~(1ULL << (idx % 64));
        if(best_bid_idx_ == idx)                                   // 只有空掉的正好是盘口才要重算
            best_bid_idx_ = (idx == 0) ? kNone : next_down(buy_occupied_, idx - 1);
    } else {
        sell_occupied_[idx/64] &= ~(1ULL << (idx % 64));
        if(best_ask_idx_ == idx)
            best_ask_idx_ = next_up(sell_occupied_, idx + 1);       // idx+1 == kLevels 时 next_up 自己会返回哨兵
    }
}

// TODO(you): match against the opposite side, then rest whatever is left.
//
// Same shape as ArrayOrderBook::submit. Two things change: resting an order now starts with
// `alloc()` and consuming one ends with `release()`, and walking a level's queue is
// `slot = pool_[slot].next` instead of `++it`.
//
// Two ways to refuse, both returning 0 with nothing rested and no fills: a price off-grid or
// outside the window, and an empty pool. Check the pool BEFORE changing anything — an order that
// was half-applied and then refused is worse than one that was simply refused.
common::Qty FlatOrderBook::submit(const OrderRequest& req, std::vector<Fill>& fills) {
    fills.clear();

    std::size_t idx = 0;
    if(!index(req.limit_price, idx)) return 0;

    common::Qty remaining = req.qty;
    //撮合
    if(req.side == common::Side::Buy){
        std::size_t cur = best_ask_idx_;
        while (remaining > 0 && cur != kNone && cur <= idx){
            PoolLevel& lvl = sell_levels_[cur];
        
            while(remaining > 0 && lvl.head != kNil){
                const std::uint32_t top = lvl.head;
                const common::Qty fillQty = std::min(remaining, pool_[top].remainQty);
                const common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
                fills.push_back(Fill{req.ref, pool_[top].ref, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; pool_[top].remainQty -= fillQty; // 两边各减成交量
                lvl.total_qty -= fillQty;

                if(pool_[top].remainQty <= 0){
                    ref_erase(pool_[top].ref);
                    sell_orders_count--;
                    unlink(lvl, top);
                    release(top);
                }    
            }
            if(lvl.head == kNil) on_level_emptied(common::Side::Sell, cur);
            cur = best_ask_idx_;
        }
    } else {
        std::size_t cur = best_bid_idx_;
        while (remaining > 0 && cur != kNone && cur >= idx){
            PoolLevel& lvl = buy_levels_[cur];

            while(remaining > 0 && lvl.head != kNil){
                const std::uint32_t top = lvl.head;
                const common::Qty fillQty = std::min(remaining, pool_[top].remainQty);
                const common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
                fills.push_back(Fill{req.ref, pool_[top].ref, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; pool_[top].remainQty -= fillQty; // 两边各减成交量
                lvl.total_qty -= fillQty;

                if(pool_[top].remainQty <= 0){
                    ref_erase(pool_[top].ref);
                    buy_orders_count--;
                    unlink(lvl, top);
                    release(top);
                }
            }
            if(lvl.head == kNil) on_level_emptied(common::Side::Buy, cur);
            cur = best_bid_idx_;
        }
    }
    if (remaining == 0) return 0;

    // 部成挂单
    std::uint32_t slot = alloc();
    if (slot == kNil) return 0; // 池子满了

    pool_[slot].ref = req.ref;
    pool_[slot].remainQty = remaining;
    pool_[slot].level = static_cast<std::uint32_t>(idx);
    pool_[slot].side = req.side;

    // The ref table is a THIRD way this book can refuse an order, and unlike the other two it is
    // discovered late. Insert before anything is linked, so a refusal costs nothing but the slot
    // that is handed straight back; doing it after push_back would mean unwinding a live order.
    if (!ref_insert(req.ref, slot)) {
        release(slot);
        return 0;
    }

    if(req.side == common::Side::Buy){
        push_back(buy_levels_[idx], slot);
        buy_orders_count++;
    } else {
        push_back(sell_levels_[idx], slot);
        sell_orders_count++;
    }
    on_level_occupied(req.side, idx);
    
    return remaining;
}

// Remove a resting order by reference number, returning its remaining quantity.
//
// There is no locator struct to look at any more: the order carries its own `side` and `level`.
// Look up the slot, read those two fields off the order, unlink it, release the slot, and clear the
// occupancy bit if the level ended up empty.
common::Qty FlatOrderBook::cancel(common::OrderRefNum ref) {
    const std::size_t e = ref_find(ref);
    if (e == kRefTableSize) return 0;

    const std::uint32_t slot = ref_table_[e].slot;
    const common::Qty qty = pool_[slot].remainQty;
    const common::Side side = pool_[slot].side;
    const std::size_t lvl_idx = pool_[slot].level;
    PoolLevel& lvl = (side == common::Side::Buy) ? buy_levels_[lvl_idx] : sell_levels_[lvl_idx];
    unlink(lvl, slot);
    if(side == common::Side::Buy){
        buy_orders_count--;
    } else {
        sell_orders_count--;
    }
    ref_erase(ref);
    release(slot);
    if(lvl.head == kNil) on_level_emptied(side, lvl_idx);
    return qty;
}

// Highest occupied bid level, or empty when there are none. One index read.
std::optional<common::Price> FlatOrderBook::best_bid() const noexcept {
    if (best_bid_idx_ == kNone) return std::nullopt;
    return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(best_bid_idx_) * kTickSize);
}

// Lowest occupied ask level, or empty when there are none. One index read.
std::optional<common::Price> FlatOrderBook::best_ask() const noexcept {
    if (best_ask_idx_ == kNone) return std::nullopt;
    return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(best_ask_idx_) * kTickSize);
}

// Total resting quantity at one price on one side.
//
// 0 for an empty level, and also 0 for a price this book can never hold.
common::Qty FlatOrderBook::qty_at(common::Side side, common::Price price) const noexcept {
    std::size_t idx = 0;
    if(!index(price, idx)) return 0;
    if(side == common::Side::Buy){
        return buy_levels_[idx].total_qty;
    } else if (side == common::Side::Sell){
        return sell_levels_[idx].total_qty;
    }
    return 0;
}

// True when no orders rest on either side.
bool FlatOrderBook::empty() const noexcept {
    return buy_orders_count == 0 && sell_orders_count == 0;
}

// How many orders rest in the book, both sides together.
std::size_t FlatOrderBook::order_count() const noexcept {
    return buy_orders_count + sell_orders_count;
}

}  // namespace hft::matching
