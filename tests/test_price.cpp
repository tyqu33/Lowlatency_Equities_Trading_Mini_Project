// tests/test_price.cpp — unit tests for hft::common::Price.
//
//   Run just this file's cases:   ./build/bin/hft_tests --gtest_filter='PriceTest.*'
//   Run one case:                 ./build/bin/hft_tests --gtest_filter='PriceTest.RoundDown_Negative'
//
// The rounding cases are the ones with teeth: C++ integer division truncates toward zero, so a
// naive `(t / ts) * ts` passes every positive case here and fails every negative one.
// RoundingProperties sweeps [-1000, 1000] asserting the four invariants that pin the behaviour down
// (bracketing, lands on a tick, moves less than one tick, idempotent).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "hft/common/price.hpp"

using hft::common::kPriceScale;
using hft::common::Price;

namespace {

// The standard US equities minimum increment above $1.00 is $0.01 (Rule 612).
// Expressed in ticks: $0.01 * 10'000 = 100.
constexpr std::int64_t kPennyTick = 100;

}  // namespace

// ============================================================================
// Construction, accessors, comparison — defined inline in price.hpp
// ============================================================================

TEST(PriceTest, FromTicksRoundTrip) {
    EXPECT_EQ(Price::from_ticks(1'502'500).ticks(), 1'502'500);
    EXPECT_EQ(Price::from_ticks(0).ticks(), 0);
    EXPECT_EQ(Price::from_ticks(-15'000).ticks(), -15'000);
}

TEST(PriceTest, DefaultAndZeroAreZero) {
    EXPECT_EQ(Price{}.ticks(), 0);
    EXPECT_EQ(Price::zero().ticks(), 0);
    EXPECT_EQ(Price{}, Price::zero());
}

TEST(PriceTest, Comparison) {
    const Price a = Price::from_ticks(1'502'500);  // $150.25
    const Price b = Price::from_ticks(1'502'600);  // $150.26
    const Price a2 = Price::from_ticks(1'502'500);

    EXPECT_EQ(a, a2);
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
    EXPECT_LE(a, b);
    EXPECT_LE(a, a2);
    EXPECT_GT(b, a);
    EXPECT_GE(b, a);
    EXPECT_GE(a, a2);
}

TEST(PriceTest, ComparisonHandlesNegatives) {
    EXPECT_LT(Price::from_ticks(-100), Price::from_ticks(0));
    EXPECT_LT(Price::from_ticks(-200), Price::from_ticks(-100));
    EXPECT_GT(Price::from_ticks(1), Price::from_ticks(-1));
}

TEST(PriceTest, OffsetTicks) {
    const Price p = Price::from_ticks(1'502'500);
    EXPECT_EQ(p.offset_ticks(100).ticks(), 1'502'600);
    EXPECT_EQ(p.offset_ticks(-100).ticks(), 1'502'400);
    EXPECT_EQ(p.offset_ticks(0), p);
    // offset is not mutating — p itself is unchanged
    EXPECT_EQ(p.ticks(), 1'502'500);
}

// operator- yields a signed spread in ticks, NOT a Price. Subtracting two prices gives a distance,
// which is a different kind of thing — that distinction is the whole reason Price is a class.
TEST(PriceTest, DifferenceIsASignedTickCount) {
    const Price bid = Price::from_ticks(1'502'500);
    const Price ask = Price::from_ticks(1'502'700);

    EXPECT_EQ(ask - bid, 200);   // 2-cent spread
    EXPECT_EQ(bid - ask, -200);  // signed
    EXPECT_EQ(bid - bid, 0);

    static_assert(std::is_same_v<decltype(ask - bid), std::int64_t>,
                  "price difference must be a tick count, not a Price");
}

TEST(PriceTest, ToDollars) {
    EXPECT_DOUBLE_EQ(Price::from_ticks(1'502'500).to_dollars(), 150.25);
    EXPECT_DOUBLE_EQ(Price::from_ticks(10'000).to_dollars(), 1.0);
    EXPECT_DOUBLE_EQ(Price::from_ticks(1).to_dollars(), 0.0001);
    EXPECT_DOUBLE_EQ(Price::from_ticks(0).to_dollars(), 0.0);
    EXPECT_DOUBLE_EQ(Price::from_ticks(-15'000).to_dollars(), -1.5);
}

// Because these are constexpr, they also hold at compile time: if any were wrong this file would
// fail to COMPILE, before a single test runs.
static_assert(Price::from_ticks(1'502'500).ticks() == 1'502'500);
static_assert(Price::from_ticks(100).offset_ticks(-100) == Price::zero());
static_assert(Price::from_ticks(200) - Price::from_ticks(50) == 150);
static_assert(sizeof(Price) == sizeof(std::int64_t));

// ============================================================================
// Tick alignment — price.hpp
// ============================================================================

TEST(PriceTest, IsOnTick) {
    EXPECT_TRUE(Price::from_ticks(1'502'500).is_on_tick(kPennyTick));   // $150.25 — on a penny
    EXPECT_FALSE(Price::from_ticks(1'502'550).is_on_tick(kPennyTick));  // $150.2550 — sub-penny
    EXPECT_TRUE(Price::from_ticks(0).is_on_tick(kPennyTick));           // zero is on every tick
    EXPECT_TRUE(Price::from_ticks(-1'502'500).is_on_tick(kPennyTick));  // negatives too
    EXPECT_FALSE(Price::from_ticks(-1'502'550).is_on_tick(kPennyTick));
    EXPECT_TRUE(Price::from_ticks(7).is_on_tick(1));  // tick_size 1: everything is on tick
}

TEST(PriceTest, RoundDown_Positive) {
    EXPECT_EQ(Price::from_ticks(150).round_down_to_tick(kPennyTick).ticks(), 100);
    EXPECT_EQ(Price::from_ticks(199).round_down_to_tick(kPennyTick).ticks(), 100);
    EXPECT_EQ(Price::from_ticks(200).round_down_to_tick(kPennyTick).ticks(), 200);  // already on tick
    EXPECT_EQ(Price::from_ticks(0).round_down_to_tick(kPennyTick).ticks(), 0);
}

TEST(PriceTest, RoundUp_Positive) {
    EXPECT_EQ(Price::from_ticks(150).round_up_to_tick(kPennyTick).ticks(), 200);
    EXPECT_EQ(Price::from_ticks(101).round_up_to_tick(kPennyTick).ticks(), 200);
    EXPECT_EQ(Price::from_ticks(200).round_up_to_tick(kPennyTick).ticks(), 200);  // already on tick
    EXPECT_EQ(Price::from_ticks(0).round_up_to_tick(kPennyTick).ticks(), 0);
}

// THE case that catches the truncation bug. `(t / ts) * ts` passes every positive test above and
// fails every one of these.
TEST(PriceTest, RoundDown_Negative) {
    EXPECT_EQ(Price::from_ticks(-150).round_down_to_tick(kPennyTick).ticks(), -200);
    EXPECT_EQ(Price::from_ticks(-101).round_down_to_tick(kPennyTick).ticks(), -200);
    EXPECT_EQ(Price::from_ticks(-200).round_down_to_tick(kPennyTick).ticks(), -200);  // on tick
    EXPECT_EQ(Price::from_ticks(-1).round_down_to_tick(kPennyTick).ticks(), -100);
}

TEST(PriceTest, RoundUp_Negative) {
    EXPECT_EQ(Price::from_ticks(-150).round_up_to_tick(kPennyTick).ticks(), -100);
    EXPECT_EQ(Price::from_ticks(-199).round_up_to_tick(kPennyTick).ticks(), -100);
    EXPECT_EQ(Price::from_ticks(-200).round_up_to_tick(kPennyTick).ticks(), -200);  // on tick
    EXPECT_EQ(Price::from_ticks(-1).round_up_to_tick(kPennyTick).ticks(), 0);
}

// Property: rounding must bracket the original value, land on a tick, and be idempotent.
// This catches sign bugs across the whole range without enumerating cases by hand.
TEST(PriceTest, RoundingProperties) {
    for (std::int64_t t = -1000; t <= 1000; ++t) {
        const Price p = Price::from_ticks(t);
        const Price down = p.round_down_to_tick(kPennyTick);
        const Price up = p.round_up_to_tick(kPennyTick);

        EXPECT_LE(down, p) << "t = " << t;
        EXPECT_GE(up, p) << "t = " << t;
        EXPECT_TRUE(down.is_on_tick(kPennyTick)) << "t = " << t;
        EXPECT_TRUE(up.is_on_tick(kPennyTick)) << "t = " << t;
        EXPECT_LT(p - down, kPennyTick) << "t = " << t;  // moved less than one tick
        EXPECT_LT(up - p, kPennyTick) << "t = " << t;

        // Idempotent: rounding an already-rounded price changes nothing.
        EXPECT_EQ(down.round_down_to_tick(kPennyTick), down) << "t = " << t;
        EXPECT_EQ(up.round_up_to_tick(kPennyTick), up) << "t = " << t;

        // On-tick values are fixed points of BOTH directions.
        if (p.is_on_tick(kPennyTick)) {
            EXPECT_EQ(down, p) << "t = " << t;
            EXPECT_EQ(up, p) << "t = " << t;
        }
    }
}

// ============================================================================
// from_dollars — price.cpp
// ============================================================================

TEST(PriceTest, FromDollars) {
    EXPECT_EQ(Price::from_dollars(150.25).ticks(), 1'502'500);
    EXPECT_EQ(Price::from_dollars(1.0).ticks(), 10'000);
    EXPECT_EQ(Price::from_dollars(0.0001).ticks(), 1);
    EXPECT_EQ(Price::from_dollars(0.0).ticks(), 0);
    EXPECT_EQ(Price::from_dollars(-1.5).ticks(), -15'000);
}

// Rounds to the NEAREST tick rather than truncating. A cast-based implementation fails here.
TEST(PriceTest, FromDollarsRoundsToNearestTick) {
    EXPECT_EQ(Price::from_dollars(0.00004).ticks(), 0);   // rounds down to 0
    EXPECT_EQ(Price::from_dollars(0.00006).ticks(), 1);   // rounds up to 1
    EXPECT_EQ(Price::from_dollars(-0.00006).ticks(), -1);
}

// ============================================================================
// from_string / to_string — price.cpp
// ============================================================================

TEST(PriceTest, FromStringValid) {
    struct Case {
        const char* in;
        std::int64_t want_ticks;
    };
    constexpr Case kCases[] = {
        {"150.25", 1'502'500},  {"150.2500", 1'502'500},
        {"150", 1'500'000},     {"0.0001", 1},
        {"0", 0},               {"0.0000", 0},
        {"-1.5", -15'000},      {"+2.5", 25'000},
        {"-0.0001", -1},        {"150.2", 1'502'000},  // NOT 150.0002 — left-pad the fraction
    };
    for (const auto& c : kCases) {
        Price out = Price::from_ticks(-999);  // poison, so we can see if it was written
        EXPECT_TRUE(Price::from_string(c.in, out)) << "input = " << c.in;
        EXPECT_EQ(out.ticks(), c.want_ticks) << "input = " << c.in;
    }
}

TEST(PriceTest, FromStringRejectsMalformed) {
    constexpr const char* kBad[] = {
        "", "abc", "1.2.3", "1.00001",  // 5 decimals — finer than a tick
        ".", "-", "+", "1.2a", " 1.5", "1.5 ",
    };
    for (const char* s : kBad) {
        Price out = Price::from_ticks(-999);
        EXPECT_FALSE(Price::from_string(s, out)) << "input = '" << s << "'";
        EXPECT_EQ(out.ticks(), -999) << "must not modify out on failure, input = '" << s << "'";
    }
}

TEST(PriceTest, ToString) {
    EXPECT_EQ(Price::from_ticks(1'502'500).to_string(), "150.2500");
    EXPECT_EQ(Price::from_ticks(10'000).to_string(), "1.0000");
    EXPECT_EQ(Price::from_ticks(1).to_string(), "0.0001");
    EXPECT_EQ(Price::from_ticks(0).to_string(), "0.0000");
    EXPECT_EQ(Price::from_ticks(-1).to_string(), "-0.0001");  // sign survives a zero integer part
    EXPECT_EQ(Price::from_ticks(-1'502'500).to_string(), "-150.2500");
}

// The real invariant: format then parse must return exactly the same price, for every price.
TEST(PriceTest, StringRoundTrip) {
    constexpr std::int64_t kSamples[] = {
        0, 1, -1, 10'000, -10'000, 1'502'500, -1'502'500, 9'999, 123'456'789,
    };
    for (std::int64_t t : kSamples) {
        const Price original = Price::from_ticks(t);
        Price parsed;
        ASSERT_TRUE(Price::from_string(original.to_string(), parsed)) << "ticks = " << t;
        EXPECT_EQ(parsed, original) << "ticks = " << t << ", text = " << original.to_string();
    }
}

// ============================================================================
// Scale sanity
// ============================================================================

TEST(PriceTest, ScaleIsFourImpliedDecimals) {
    EXPECT_EQ(kPriceScale, 10'000);
    EXPECT_EQ(Price::from_ticks(kPriceScale).to_dollars(), 1.0);
}
