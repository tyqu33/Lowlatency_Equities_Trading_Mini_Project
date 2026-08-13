// libs/matching/src/order_book.cpp
// Responsibility: price-time-priority matching and book maintenance.
//
// See the header for the contract and for the note on the three data structures.

#include "hft/matching/order_book.hpp"

namespace hft::matching {

// Match against the opposite side, then rest the remainder.
//
// The shape of it:
//   1. Walk the opposite side from the best price outward, while the incoming order can trade with
//      that level (for a limit order: is the level's price within the limit?) and still has
//      quantity left.
//   2. At each level, consume orders from the FRONT — that is time priority. A resting order that
//      fills completely leaves the book (and its entry in the ref index goes with it); one that
//      fills partially stays where it is, keeping its place in line.
//   3. Emit a Fill per (aggressor, resting) pair, priced at the RESTING order's price.
//   4. Whatever quantity is left rests at the incoming order's own limit price.
//
// Watch for: a level that empties must be removed, or `best_bid`/`best_ask` will report a price
// with nothing behind it.
common::Qty OrderBook::submit(const OrderRequest& req, std::vector<Fill>& fills) {
    fills.clear();
    common::Qty remaining = req.qty;
    if(req.side == common::Side::Buy){
        while(remaining > 0 && !sell.empty()){
            auto it = sell.begin();
            if(it->first > req.limit_price) break; //卖档的最低价都高于买方限价
            while(remaining > 0 && !it->second.orders.empty()){
                auto& orders = it->second.orders;
                common::Qty fillQty = std::min(remaining, orders.front().remainQty);
                fills.push_back(Fill{req.ref, orders.front().resting, it->first, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; orders.front().remainQty -= fillQty; // 两边各减成交量
                it->second.total_qty -= fillQty;

                if(orders.front().remainQty <= 0){
                    refMap.erase(orders.front().resting); sell_orders_count--;
                    orders.pop_front();
                }
            }
            if(it->second.orders.empty()){
                sell.erase(it);
            }
        }
    } else if(req.side == common::Side::Sell){
        while(remaining > 0 && !buy.empty()){
            auto it = buy.begin();
            if(it->first < req.limit_price) break; //买档的最高价都低于卖方限价
            while(remaining > 0 && !it->second.orders.empty()){
                auto& orders = it->second.orders;
                common::Qty fillQty = std::min(remaining, orders.front().remainQty);
                fills.push_back(Fill{req.ref, orders.front().resting, it->first, fillQty}); // 已成: 本方订单编号ref, 对手方挂单编号ref, 档价, 成交量
                remaining -= fillQty; orders.front().remainQty -= fillQty; // 两边各减成交量
                it->second.total_qty -= fillQty;

                if(orders.front().remainQty <= 0){
                    refMap.erase(orders.front().resting); buy_orders_count--;
                    orders.pop_front();
                }
            }
            if(it->second.orders.empty()){
                buy.erase(it);
            }
        }
    }

    if (remaining == 0) return 0;

    if(req.side == common::Side::Buy){ // Push the remaining qty of current order to its belonging queue
        common::Price bid_price = req.limit_price;
        
        Level& l = buy[bid_price];
        RestingOrder order; order.resting = req.ref; order.remainQty = remaining; //req.qty;
        l.total_qty += remaining;
        l.orders.push_back(order);
        Locator loc; loc.side = common::Side::Buy; loc.price = req.limit_price; loc.it = --(l.orders.end());
        refMap[req.ref] = loc;

        buy_orders_count++;
        return remaining; 
    } else if(req.side == common::Side::Sell){ // Push the remaining qty of current order to its belonging queue
        common::Price ask_price = req.limit_price;

        Level& l = sell[ask_price];
        RestingOrder order; order.resting = req.ref; order.remainQty = remaining; //req.qty;
        l.total_qty += remaining;
        l.orders.push_back(order);
        Locator loc; loc.side = common::Side::Sell; loc.price = req.limit_price; loc.it = --(l.orders.end());
        refMap[req.ref] = loc;

        sell_orders_count++;
        return remaining;
    }
    return remaining;
}

// Remove a resting order by reference number, returning its remaining quantity.
//
// This is where data structure (3) earns its place: find the order directly, read off what is left
// of it, unlink it from its level, drop the level if it is now empty, and erase the index entry.
// Returning 0 for an unknown ref is normal operation, not a failure.
common::Qty OrderBook::cancel(common::OrderRefNum ref) {
    auto it = refMap.find(ref);
    if(it == refMap.end()) return 0;
    Locator& loc = it->second;
    common::Qty qty = loc.it->remainQty;
    common::Price price = loc.price;
    common::Side side = loc.side;
    // auto& l = loc.it; // std::list<RestingOrder>::iterator
    if(side == common::Side::Buy){
        auto it_level = buy.find(price);
        if(it_level == buy.end()){ refMap.erase(ref); return 0;}
        Level& lvl = it_level->second;
        lvl.total_qty -= qty;
        lvl.orders.erase(loc.it);
        if(lvl.orders.empty()){buy.erase(it_level);}

        refMap.erase(ref);
        buy_orders_count--;
        
    } else if(side == common::Side::Sell){
        auto it_level = sell.find(price);
        if(it_level == sell.end()){ refMap.erase(ref); return 0;}
        Level& lvl = it_level->second;
        lvl.total_qty -= qty;
        lvl.orders.erase(loc.it);
        if(lvl.orders.empty()){sell.erase(it_level);}

        refMap.erase(ref);
        sell_orders_count--; 
    }
    return qty;
}

// Highest resting bid price, or empty when there are no bids.
std::optional<common::Price> OrderBook::best_bid() const noexcept { // bid = buy
    if(!buy.empty()){
        std::optional<common::Price> opt = buy.begin()->first;
        return opt;
    }
    return std::nullopt;
}

// Lowest resting ask price, or empty when there are no asks.
std::optional<common::Price> OrderBook::best_ask() const noexcept { // ask = sell
    if(!sell.empty()){
        std::optional<common::Price> opt = sell.begin()->first;
        return opt;
    }
    return std::nullopt;
}

// Total resting quantity at one price on one side; 0 when the level does not exist.
common::Qty OrderBook::qty_at(common::Side side, common::Price price) const noexcept {
    if(side == common::Side::Buy){
        auto it = buy.find(price);
        if(it != buy.end()) {
            return it->second.total_qty;
        }
    } else if(side == common::Side::Sell){
        auto it = sell.find(price);
        if(it != sell.end()){
            return it->second.total_qty;
        }
    }
    return 0;
}

// True when no orders rest on either side.
bool OrderBook::empty() const noexcept {
    return buy.empty() && sell.empty();
}

// How many orders rest in the book, both sides together.
std::size_t OrderBook::order_count() const noexcept {
    return buy_orders_count + sell_orders_count;
}

}  // namespace hft::matching
