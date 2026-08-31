// tests/test_order_book.cpp — the shared specification for every order book implementation.
//
// Every case below runs TWICE, once against each type in `BookImpls`: the std::map baseline and
// the array-indexed replacement. That is the point. An optimisation is only worth anything if the
// thing it optimised still behaves identically, and the cheapest way to hold that line is to give
// both implementations the same suite rather than a parallel one that can drift.
//
// Adding an implementation means adding it to `BookImpls`; nothing else here changes.
//
//   Run all of them:   ./build/bin/hft_tests --gtest_filter='OrderBook*'
//   One implementation:./build/bin/hft_tests --gtest_filter='*/1.*'      (0 = map, 1 = array)
//   One section:       ./build/bin/hft_tests --gtest_filter='OrderBookCancel/*'
//   One case:          ./build/bin/hft_tests --gtest_filter='*RestsRemainderAtItsOwnLimit*'
//
// All prices here sit on the one-cent tick grid. That is not decoration: the array implementation
// indexes levels by grid step and rejects anything off it, so an off-grid price would make the two
// implementations legitimately disagree and the shared suite meaningless.
//
// The three cases most likely to catch a subtly wrong implementation:
//   - FillsAtRestingPrice            — the aggressor gets the price improvement, not the resting side
//   - RestsRemainderAtItsOwnLimit    — leftover rests at the order's limit, not at the price it traded
//   - TimePriorityWithinLevel        — first in, first filled, and a partial fill keeps its place

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "hft/common/price.hpp"
#include "hft/common/types.hpp"
#include "hft/matching/array_order_book.hpp"
#include "hft/matching/pool_order_book.hpp"
#include "hft/matching/order_book.hpp"

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
using hft::matching::PoolOrderBook;

// Every implementation of the book. Index 0 is the baseline, 1 the array version.
using BookImpls = ::testing::Types<OrderBook, ArrayOrderBook, PoolOrderBook>;

namespace {

// 1 tick = 1/10000 USD, so one cent = 100 ticks.
constexpr std::int64_t kPx99_98 = 999'800;
constexpr std::int64_t kPx99_99 = 999'900;
constexpr std::int64_t kPx100_00 = 1'000'000;
constexpr std::int64_t kPx100_01 = 1'000'100;
constexpr std::int64_t kPx100_02 = 1'000'200;

OrderRequest limit(OrderRefNum ref, Side side, std::int64_t price_ticks, Qty qty) {
    OrderRequest r{};
    r.ref = ref;
    r.side = side;
    r.type = OrderType::Limit;
    r.tif = TimeInForce::Day;
    r.qty = qty;
    r.limit_price = Price::from_ticks(price_ticks);
    return r;
}

// Submit and discard the fills — for setting up book state.
template <typename Book>
Qty rest(Book& book, const OrderRequest& req) {
    std::vector<Fill> fills;
    return book.submit(req, fills);
}

// A price `levels` one-cent steps away from $100.0000, so randomised traffic stays on the grid.
std::int64_t grid_price(int levels) {
    return kPx100_00 + static_cast<std::int64_t>(levels) * 100;
}

}  // namespace

// A typed suite needs a fixture template even when it holds no state; the test body reaches the
// implementation under test through `TypeParam`.
template <typename Book>
class OrderBookBasics : public ::testing::Test {};
template <typename Book>
class OrderBookResting : public ::testing::Test {};
template <typename Book>
class OrderBookMatching : public ::testing::Test {};
template <typename Book>
class OrderBookCancel : public ::testing::Test {};
template <typename Book>
class OrderBookLevels : public ::testing::Test {};
template <typename Book>
class OrderBookInvariants : public ::testing::Test {};

// Each suite is instantiated once per implementation. GoogleTest names the results
// `SuiteName/0.CaseName` for the first type in BookImpls and `SuiteName/1.CaseName` for the second.
TYPED_TEST_SUITE(OrderBookBasics, BookImpls);
TYPED_TEST_SUITE(OrderBookResting, BookImpls);
TYPED_TEST_SUITE(OrderBookMatching, BookImpls);
TYPED_TEST_SUITE(OrderBookCancel, BookImpls);
TYPED_TEST_SUITE(OrderBookLevels, BookImpls);
TYPED_TEST_SUITE(OrderBookInvariants, BookImpls);

// ============================================================================
// An empty book
// ============================================================================

TYPED_TEST(OrderBookBasics, StartsEmpty) {
    TypeParam book;
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 0);
    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_00)), 0);
}

TYPED_TEST(OrderBookBasics, CancelOnEmptyBookReportsNothingRemoved) {
    TypeParam book;
    EXPECT_EQ(book.cancel(12345), 0);
}

// ============================================================================
// Resting: an order that cannot trade goes into the book
// ============================================================================

TYPED_TEST(OrderBookResting, UnmatchableBuyRests) {
    TypeParam book;
    std::vector<Fill> fills;

    EXPECT_EQ(book.submit(limit(1, Side::Buy, kPx100_00, 500), fills), 500)
        << "nothing to trade with, so the whole order rests";
    EXPECT_TRUE(fills.empty());

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.order_count(), 1u);
    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx100_00));
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 500);
}

TYPED_TEST(OrderBookResting, UnmatchableSellRests) {
    TypeParam book;
    EXPECT_EQ(rest(book, limit(1, Side::Sell, kPx100_01, 300)), 300);

    EXPECT_EQ(book.best_ask(), Price::from_ticks(kPx100_01));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_01)), 300);
}

TYPED_TEST(OrderBookResting, BestBidIsHighestBestAskIsLowest) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx99_98, 100));
    rest(book, limit(2, Side::Buy, kPx100_00, 100));  // better bid, submitted second
    rest(book, limit(3, Side::Buy, kPx99_99, 100));

    rest(book, limit(4, Side::Sell, kPx100_02, 100));
    rest(book, limit(5, Side::Sell, kPx100_01, 100));  // better ask, submitted second

    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx100_00)) << "highest bid wins";
    EXPECT_EQ(book.best_ask(), Price::from_ticks(kPx100_01)) << "lowest ask wins";
    EXPECT_EQ(book.order_count(), 5u);
}

TYPED_TEST(OrderBookResting, QtyAtAggregatesOrdersOnTheSameLevel) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 100));
    rest(book, limit(2, Side::Buy, kPx100_00, 250));
    rest(book, limit(3, Side::Buy, kPx99_99, 700));

    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 350);
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx99_99)), 700);
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx99_98)), 0) << "no such level";
    EXPECT_EQ(book.order_count(), 3u);
}

TYPED_TEST(OrderBookResting, SidesAreSeparate) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx99_99, 100));
    rest(book, limit(2, Side::Sell, kPx100_01, 100));

    // Same price, wrong side — must not see the other side's quantity.
    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx99_99)), 0);
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_01)), 0);
}

// ============================================================================
// Price priority: match the best price on the opposite side first
// ============================================================================

TYPED_TEST(OrderBookMatching, BuyTakesLowestAskFirst) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_02, 100));
    rest(book, limit(2, Side::Sell, kPx100_00, 100));  // best ask
    rest(book, limit(3, Side::Sell, kPx100_01, 100));

    std::vector<Fill> fills;
    EXPECT_EQ(book.submit(limit(4, Side::Buy, kPx100_02, 100), fills), 0);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting, 2u) << "the 100.00 ask, not the one submitted first";
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00));
    EXPECT_EQ(fills[0].qty, 100);
}

TYPED_TEST(OrderBookMatching, SellTakesHighestBidFirst) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx99_98, 100));
    rest(book, limit(2, Side::Buy, kPx100_00, 100));  // best bid
    rest(book, limit(3, Side::Buy, kPx99_99, 100));

    std::vector<Fill> fills;
    EXPECT_EQ(book.submit(limit(4, Side::Sell, kPx99_98, 100), fills), 0);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting, 2u);
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00));
}

TYPED_TEST(OrderBookMatching, WalksMultipleLevelsInPriceOrder) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));
    rest(book, limit(2, Side::Sell, kPx100_01, 200));
    rest(book, limit(3, Side::Sell, kPx100_02, 400));

    std::vector<Fill> fills;
    // Wants 350 at up to 100.01: takes all of 100.00, then 250 of the 200 available at 100.01 —
    // only 200 are there, so 300 total, and the remaining 50 rests. 100.02 is above the limit.
    EXPECT_EQ(book.submit(limit(4, Side::Buy, kPx100_01, 350), fills), 50);

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00)) << "cheapest level first";
    EXPECT_EQ(fills[0].qty, 100);
    EXPECT_EQ(fills[1].price, Price::from_ticks(kPx100_01));
    EXPECT_EQ(fills[1].qty, 200);

    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_02)), 400) << "untouched, above limit";
    EXPECT_EQ(book.best_ask(), Price::from_ticks(kPx100_02));
}

TYPED_TEST(OrderBookMatching, DoesNotTradeThroughTheLimit) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_02, 100));

    std::vector<Fill> fills;
    // Willing to pay 100.01; the only ask is 100.02. No trade.
    EXPECT_EQ(book.submit(limit(2, Side::Buy, kPx100_01, 100), fills), 100);
    EXPECT_TRUE(fills.empty());

    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx100_01));
    EXPECT_EQ(book.best_ask(), Price::from_ticks(kPx100_02));
}

TYPED_TEST(OrderBookMatching, EqualPricesDoCross) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));

    std::vector<Fill> fills;
    EXPECT_EQ(book.submit(limit(2, Side::Buy, kPx100_00, 100), fills), 0)
        << "a limit is a bound, not a strict inequality: 100.00 meets 100.00";
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00));
    EXPECT_TRUE(book.empty());
}

// ============================================================================
// Time priority within a level
// ============================================================================

TYPED_TEST(OrderBookMatching, TimePriorityWithinLevel) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));  // first in
    rest(book, limit(2, Side::Sell, kPx100_00, 100));
    rest(book, limit(3, Side::Sell, kPx100_00, 100));

    std::vector<Fill> fills;
    EXPECT_EQ(book.submit(limit(4, Side::Buy, kPx100_00, 250), fills), 0);

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].resting, 1u) << "arrival order decides, not order id or size";
    EXPECT_EQ(fills[0].qty, 100);
    EXPECT_EQ(fills[1].resting, 2u);
    EXPECT_EQ(fills[1].qty, 100);
    EXPECT_EQ(fills[2].resting, 3u);
    EXPECT_EQ(fills[2].qty, 50) << "the third is only partly consumed";

    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_00)), 50);
    EXPECT_EQ(book.order_count(), 1u) << "the first two left the book";
}

TYPED_TEST(OrderBookMatching, PartiallyFilledOrderKeepsItsPlaceInLine) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));
    rest(book, limit(2, Side::Sell, kPx100_00, 100));

    std::vector<Fill> fills;
    book.submit(limit(3, Side::Buy, kPx100_00, 60), fills);  // eats 60 of order 1

    fills.clear();
    book.submit(limit(4, Side::Buy, kPx100_00, 60), fills);  // must finish order 1 first

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting, 1u) << "a partial fill does not send you to the back of the queue";
    EXPECT_EQ(fills[0].qty, 40);
    EXPECT_EQ(fills[1].resting, 2u);
    EXPECT_EQ(fills[1].qty, 20);
}

// ============================================================================
// Fill pricing
// ============================================================================

TYPED_TEST(OrderBookMatching, FillsAtRestingPrice) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));  // resting first, sets the terms

    std::vector<Fill> fills;
    book.submit(limit(2, Side::Buy, kPx100_02, 100), fills);  // willing to pay more

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00))
        << "the buyer keeps the improvement; the resting order named the price";
    EXPECT_EQ(fills[0].aggressor, 2u);
    EXPECT_EQ(fills[0].resting, 1u);
}

TYPED_TEST(OrderBookMatching, FillsAtRestingPriceWhenSellerIsAggressor) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 100));

    std::vector<Fill> fills;
    book.submit(limit(2, Side::Sell, kPx99_98, 100), fills);  // willing to accept less

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, Price::from_ticks(kPx100_00))
        << "symmetric: now the seller gets the improvement";
    EXPECT_EQ(fills[0].aggressor, 2u);
    EXPECT_EQ(fills[0].resting, 1u);
}

// ============================================================================
// Partial fills and the resting remainder
// ============================================================================

TYPED_TEST(OrderBookMatching, IncomingSmallerThanRestingLeavesRestingReduced) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 500));

    std::vector<Fill> fills;
    EXPECT_EQ(book.submit(limit(2, Side::Buy, kPx100_00, 200), fills), 0);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].qty, 200);
    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_00)), 300);
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(OrderBookMatching, RestsRemainderAtItsOwnLimit) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));

    std::vector<Fill> fills;
    // Buy 300 at up to 100.02. Trades 100 at 100.00; the other 200 rest — at 100.02, the order's
    // own limit, NOT at 100.00, the price it happened to trade at.
    EXPECT_EQ(book.submit(limit(2, Side::Buy, kPx100_02, 300), fills), 200);

    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx100_02));
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_02)), 200);
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 0);
}

// ============================================================================
// Cancel
// ============================================================================

TYPED_TEST(OrderBookCancel, RemovesRestingOrderAndReportsItsQuantity) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 400));

    EXPECT_EQ(book.cancel(1), 400);
    EXPECT_TRUE(book.empty());
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 0);
}

TYPED_TEST(OrderBookCancel, UnknownRefReportsNothingRemoved) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 400));

    EXPECT_EQ(book.cancel(999), 0) << "not an error — the order may never have existed";
    EXPECT_EQ(book.order_count(), 1u) << "and the book is untouched";
}

TYPED_TEST(OrderBookCancel, TwiceReportsNothingTheSecondTime) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 400));

    EXPECT_EQ(book.cancel(1), 400);
    EXPECT_EQ(book.cancel(1), 0) << "gone is gone";
}

TYPED_TEST(OrderBookCancel, AfterPartialFillReportsOnlyWhatIsLeft) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 1000));

    std::vector<Fill> fills;
    book.submit(limit(2, Side::Buy, kPx100_00, 300), fills);

    EXPECT_EQ(book.cancel(1), 700) << "remaining quantity, not the original 1000";
    EXPECT_TRUE(book.empty());
}

TYPED_TEST(OrderBookCancel, FilledOrderIsNoLongerCancellable) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));

    std::vector<Fill> fills;
    book.submit(limit(2, Side::Buy, kPx100_00, 100), fills);  // consumes order 1 entirely

    EXPECT_EQ(book.cancel(1), 0) << "it left the book when it filled";
    EXPECT_EQ(book.cancel(2), 0) << "the aggressor never rested";
}

TYPED_TEST(OrderBookCancel, LeavesSiblingsOnTheSameLevelAlone) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 100));
    rest(book, limit(2, Side::Buy, kPx100_00, 250));
    rest(book, limit(3, Side::Buy, kPx100_00, 50));

    EXPECT_EQ(book.cancel(2), 250) << "removing from the middle of the queue";
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 150);
    EXPECT_EQ(book.order_count(), 2u);
    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx100_00)) << "level survives";

    // The survivors keep their relative order.
    std::vector<Fill> fills;
    book.submit(limit(4, Side::Sell, kPx100_00, 150), fills);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting, 1u);
    EXPECT_EQ(fills[1].resting, 3u);
}

// ============================================================================
// Level lifecycle — an emptied level must disappear
// ============================================================================

TYPED_TEST(OrderBookLevels, CancellingTheLastOrderRemovesTheLevel) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx100_00, 100));
    rest(book, limit(2, Side::Buy, kPx99_99, 100));

    EXPECT_EQ(book.cancel(1), 100);
    EXPECT_EQ(book.best_bid(), Price::from_ticks(kPx99_99))
        << "best bid must fall back to the next level, not report an empty one";
    EXPECT_EQ(book.qty_at(Side::Buy, Price::from_ticks(kPx100_00)), 0);
}

TYPED_TEST(OrderBookLevels, FullyConsumedLevelRemovesItself) {
    TypeParam book;
    rest(book, limit(1, Side::Sell, kPx100_00, 100));
    rest(book, limit(2, Side::Sell, kPx100_01, 100));

    std::vector<Fill> fills;
    book.submit(limit(3, Side::Buy, kPx100_00, 100), fills);  // eats all of 100.00

    EXPECT_EQ(book.best_ask(), Price::from_ticks(kPx100_01));
    EXPECT_EQ(book.qty_at(Side::Sell, Price::from_ticks(kPx100_00)), 0);
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(OrderBookLevels, BookReturnsToEmptyAfterEverythingLeaves) {
    TypeParam book;
    rest(book, limit(1, Side::Buy, kPx99_99, 100));
    rest(book, limit(2, Side::Sell, kPx100_01, 100));

    EXPECT_EQ(book.cancel(1), 100);
    EXPECT_EQ(book.cancel(2), 100);

    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

// ============================================================================
// Invariants under random traffic
// ============================================================================

// Randomised operations, checking after every one that the book has not entered a state it should
// be unable to reach. This is the test that catches the bugs the hand-written cases above do not
// anticipate — a level left behind empty, a stale best price, a ref index out of step with the
// levels. Seeded, so a failure reproduces exactly.
TYPED_TEST(OrderBookInvariants, HoldUnderRandomTraffic) {
    TypeParam book;
    std::mt19937 rng(20260812);
    std::uniform_int_distribution<int> level_dist(-3, 3);  // 见下方 grid_price()
    std::uniform_int_distribution<Qty> qty_dist(1, 500);
    std::uniform_int_distribution<int> coin(0, 1);

    std::vector<OrderRefNum> live;
    std::vector<Fill> fills;
    OrderRefNum next_ref = 1;
    int steps_with_a_populated_book = 0;

    for (int step = 0; step < 3000; ++step) {
        const bool do_cancel = !live.empty() && coin(rng) == 0;

        if (do_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t i = pick(rng);
            const OrderRefNum ref = live[i];
            live[i] = live.back();
            live.pop_back();

            // May legitimately return 0: the order could have been filled by an aggressor since it
            // rested, in which case it is already gone. Either answer is correct; a negative one
            // never is.
            EXPECT_GE(book.cancel(ref), 0) << "step " << step;
        } else {
            const OrderRefNum ref = next_ref++;
            const Side side = coin(rng) == 0 ? Side::Buy : Side::Sell;
            if (book.submit(limit(ref, side, grid_price(level_dist(rng)), qty_dist(rng)), fills) > 0) {
                live.push_back(ref);
            }
        }

        // The invariant that matters: a book must never be crossed. If the best bid is at or above
        // the best ask, two orders that should have traded are both still sitting there.
        const std::optional<Price> bid = book.best_bid();
        const std::optional<Price> ask = book.best_ask();
        if (bid && ask) {
            EXPECT_LT(*bid, *ask) << "crossed book at step " << step;
        }

        // A reported best price must have quantity behind it — the classic symptom of a level that
        // was emptied but not removed.
        if (bid) {
            EXPECT_GT(book.qty_at(Side::Buy, *bid), 0) << "empty bid level at step " << step;
        }
        if (ask) {
            EXPECT_GT(book.qty_at(Side::Sell, *ask), 0) << "empty ask level at step " << step;
        }

        // empty() and order_count() must agree with each other and with the best prices.
        EXPECT_EQ(book.empty(), book.order_count() == 0u) << "step " << step;
        if (book.empty()) {
            EXPECT_FALSE(bid.has_value()) << "step " << step;
            EXPECT_FALSE(ask.has_value()) << "step " << step;
        } else {
            ++steps_with_a_populated_book;
        }
    }

    // Without this the whole test is a false green: every invariant above is trivially satisfied by
    // a book that is always empty, which is exactly what an unimplemented stub gives you. This
    // asserts the traffic above actually built up state to check.
    EXPECT_GT(steps_with_a_populated_book, 2000)
        << "the book never filled up — the invariants above proved nothing";
}
