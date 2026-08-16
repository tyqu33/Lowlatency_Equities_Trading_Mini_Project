// libs/matching/src/array_order_book.cpp
// Responsibility: matching and book maintenance over a flat, price-indexed array of levels.
//
// Every function here is TODO(you). They are stubbed so the library links and the shared test
// suite runs RED against this implementation while passing against OrderBook — delete each
// `// TODO` marker as you implement it. See the header for the window rules and for the note on
// finding the best price without a std::map to ask.

#include <bit>
#include "hft/matching/array_order_book.hpp"


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


    bool ArrayOrderBook::index(common::Price price, std::size_t& out) noexcept{
        int64_t price_tick = price.ticks();
        if(price_tick < ArrayOrderBook::kBaseTicks || price_tick > ArrayOrderBook::kCeilTicks) return false;
        if((price_tick - kBaseTicks) % kTickSize != 0) return false;
        out = static_cast<std::size_t>(price_tick - ArrayOrderBook::kBaseTicks) / ArrayOrderBook::kTickSize;
        return true;
    }

// TODO(you): match against the opposite side, then rest the remainder.
//
// The matching loop is the same shape as OrderBook's — walk the opposite side outward from the
// touch, consume each level from the front, fill at the resting price, rest what is left at the
// incoming order's own limit. Two things differ:
//
//   - Getting the next level is an index step, not `++it` on a map iterator. Walking outward means
//     moving the index down for bids and up for asks, and skipping unoccupied levels (this is what
//     the bitmap is for; a linear scan over empty levels would be worse than the tree you just
//     replaced).
//   - A level that empties is not erased. Clear its bit and leave it be.
//
// Reject the order — return 0, rest nothing, emit no fills — when the price is off-grid or outside
// the window.
common::Qty ArrayOrderBook::submit(const OrderRequest& req, std::vector<Fill>& fills) {
    fills.clear();
    common::Qty remaining = req.qty;

    std::size_t idx = 0;
    if(!index(req.limit_price, idx)) return 0;

    if(req.side == common::Side::Buy){
        std::size_t cur = next_up(sell_occupied_, 0);

        while(remaining > 0 && cur <= idx){
            // if(sell_occupied_[w] == 0) {w++; continue;}
            // std::size_t bit = static_cast<std::size_t>(std::countr_zero(sell_occupied_[w]));
            // std::size_t cur = w * 64 + bit;
            Level& l = sell_levels_[cur];

            while(remaining > 0 && !l.orders.empty()){
                auto& top = l.orders.front();
            
                common::Qty fillQty = std::min(remaining, top.remainQty);
                common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
                fills.push_back(Fill{req.ref, top.resting, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; top.remainQty -= fillQty; // 两边各减成交量
                l.total_qty -= fillQty;

                if(top.remainQty <= 0){
                    refMap.erase(top.resting);
                    sell_orders_count--;
                    l.orders.pop_front();
                }
            }
            if(l.orders.empty()){
                sell_occupied_[cur/64] &= ~(1ULL << (cur % 64));
            }
            cur = next_up(sell_occupied_, cur + 1);
        }
        // // older version: traversal
        // while(remaining > 0 && cur <= idx){
        //     Level& l = sell_levels_[cur];
        //     if(l.orders.empty()) {cur++; continue;}
        //     while(remaining > 0 && !l.orders.empty()){
        //         auto& top = l.orders.front();
            
        //         common::Qty fillQty = std::min(remaining, top.remainQty);
        //         common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
        //         fills.push_back(Fill{req.ref, top.resting, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
        //         remaining -= fillQty; top.remainQty -= fillQty; // 两边各减成交量
        //         l.total_qty -= fillQty;

        //         if(top.remainQty <= 0){
        //             refMap.erase(top.resting);
        //             sell_orders_count--;
        //             l.orders.pop_front();
        //         }
        //     }
        //     if(l.orders.empty()){
        //         sell_occupied_[cur/64] &= ~(1ULL << (cur % 64));
        //     }
        //     cur++;
        // }

    } else if(req.side == common::Side::Sell) {
        std::size_t cur = next_down(buy_occupied_, kLevels - 1);

        while(remaining > 0 && cur >= idx && cur != kLevels){
            Level& l = buy_levels_[cur];

            while(remaining > 0 && !l.orders.empty()){
                auto& top = l.orders.front();
            
                common::Qty fillQty = std::min(remaining, top.remainQty);
                common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
                fills.push_back(Fill{req.ref, top.resting, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; top.remainQty -= fillQty; // 两边各减成交量
                l.total_qty -= fillQty;

                if(top.remainQty <= 0){
                    refMap.erase(top.resting);
                    buy_orders_count--;
                    l.orders.pop_front();
                }
            }
            if(l.orders.empty()){
                buy_occupied_[cur/64] &= ~(1ULL << (cur % 64)); // 置位, 清位
            }
            if(cur == 0) break;
            cur = next_down(buy_occupied_, cur - 1);
        }
        // // older version: traversal
        // while(remaining > 0 && cur >= idx){
        //     Level& l = buy_levels_[cur];
        //     if(l.orders.empty()) {cur--; continue;}
        //     while(remaining > 0 && !l.orders.empty()){
        //         auto& top = l.orders.front();
            
        //         common::Qty fillQty = std::min(remaining, top.remainQty);
        //         common::Price fillPrice = common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(cur) * kTickSize);
        //         fills.push_back(Fill{req.ref, top.resting, fillPrice, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
        //         remaining -= fillQty; top.remainQty -= fillQty; // 两边各减成交量
        //         l.total_qty -= fillQty;

        //         if(top.remainQty <= 0){
        //             refMap.erase(top.resting);
        //             buy_orders_count--;
        //             l.orders.pop_front();
        //         }
        //     }
        //     if(l.orders.empty()){
        //         buy_occupied_[cur/64] &= ~(1ULL << (cur % 64));
        //     }
        //     if(cur == 0) break;
        //     cur--;
        // }
    }

    if (remaining == 0) return 0;

    if(req.side == common::Side::Buy){
        Level& l = buy_levels_[idx];
        l.total_qty += remaining;
        l.orders.push_back({req.ref, remaining});            // std::list 分配一个节点     <- malloc

        buy_occupied_[idx/64] |= (1ULL << (idx % 64));
        buy_orders_count++;
        refMap[req.ref] = {req.side, idx, --l.orders.end()}; // unordered_map 分配一个节点 <- malloc
        
    } else if(req.side == common::Side::Sell) {
        Level& l = sell_levels_[idx];
        l.total_qty += remaining;
        l.orders.push_back({req.ref, remaining});

        sell_occupied_[idx/64] |= (1ULL << (idx % 64));
        sell_orders_count++;
        refMap[req.ref] = {req.side, idx, --l.orders.end()};
    }
    return remaining;
}

// TODO(you): remove a resting order by reference number, returning its remaining quantity.
//
// The ref index still points straight at the order, so the unlink is unchanged. What changes is
// what happens when the level goes empty: instead of erasing a map node, clear the bit — and if
// that level was the best price, the next best now has to be found from the bitmap.
common::Qty ArrayOrderBook::cancel(common::OrderRefNum ref) {
    auto it = refMap.find(ref);
    if(it == refMap.end()) return 0;

    ArrayLocator& loc = it->second;
    common::Qty qty = loc.it->remainQty;
    std::size_t idx = loc.index;
    common::Side side = loc.side;
    if(side == common::Side::Buy){
        Level& lvl = buy_levels_[idx];
        lvl.total_qty -= qty;
        lvl.orders.erase(loc.it);
        buy_orders_count--;
        refMap.erase(ref);

        if(lvl.orders.empty()){
            buy_occupied_[idx/64] &= ~(1ULL << (idx % 64));
        }
        
    } else if (side == common::Side::Sell){
        Level& lvl = sell_levels_[idx];
        lvl.total_qty -= qty;
        lvl.orders.erase(loc.it);
        sell_orders_count--;
        refMap.erase(ref);

        if(lvl.orders.empty()){
            sell_occupied_[idx/64] &= ~(1ULL << (idx % 64));
        }
    }
    return qty;
}

// TODO(you): highest occupied bid level, or empty when there are none.
std::optional<common::Price> ArrayOrderBook::best_bid() const noexcept { // bid = buy
    for(std::size_t w = kLevels / 64; w-- > 0;){
        if(buy_occupied_[w] == 0) continue;
        std::size_t bit = static_cast<std::size_t>(63 - std::countl_zero(buy_occupied_[w]));
        std::size_t idx = w * 64 + bit;
        return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(idx) * kTickSize);
    }
    return std::nullopt;
    // // older version: traversal
    // for(std::size_t i = kLevels; i-- > 0;){ // 用i>0;i--;则无符号数减到 0 以下会回绕成天文数字、死循环
    //     if(buy_levels_[i].total_qty != 0){
    //         return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(i) * kTickSize);
    //     }
    // }
    // return std::nullopt;
}

// TODO(you): lowest occupied ask level, or empty when there are none.
std::optional<common::Price> ArrayOrderBook::best_ask() const noexcept { // ask = sell
    for(std::size_t w = 0; w < kLevels / 64; w++){
        if(sell_occupied_[w] == 0) continue;
        std::size_t bit = static_cast<std::size_t>(std::countr_zero(sell_occupied_[w]));
        std::size_t idx = w * 64 + bit;
        return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(idx) * kTickSize);
    }
    return std::nullopt;
    // // older version: traversal
    // for(std::size_t i = 0; i < kLevels; i++){
    //     if(sell_levels_[i].total_qty != 0){
    //         return common::Price::from_ticks(kBaseTicks + static_cast<std::int64_t>(i) * kTickSize);
    //     }
    // }
    // return std::nullopt;
}

// TODO(you): total resting quantity at one price on one side.
//
// 0 for an empty level, and also 0 for a price that is off-grid or outside the window — those are
// prices this book can never hold, so reporting nothing at them is consistent.
common::Qty ArrayOrderBook::qty_at(common::Side side, common::Price price) const noexcept {
    std::size_t idx = 0;
    if(!index(price, idx)) return 0;
    if(side == common::Side::Buy){
        return buy_levels_[idx].total_qty;
    } else if (side == common::Side::Sell){
        return sell_levels_[idx].total_qty;
    }
    return 0;
}

// TODO(you): true when no orders rest on either side.
bool ArrayOrderBook::empty() const noexcept {
    return buy_orders_count == 0 && sell_orders_count == 0;
}

// TODO(you): how many orders rest in the book, both sides together.
std::size_t ArrayOrderBook::order_count() const noexcept {
    return buy_orders_count + sell_orders_count;
}

}  // namespace hft::matching
