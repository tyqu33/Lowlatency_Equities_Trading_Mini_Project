// tests/test_bits.cpp
// A worked GoogleTest example for hft::common bit helpers — written so you can follow the same
// pattern when testing hft::common::Price. It shows: TEST(), the common EXPECT_* macros, attaching
// a failure message with <<, a table-driven test, a small property loop, and constexpr
// static_assert.

#include <gtest/gtest.h>

#include <cstdint>

#include "hft/common/bits.hpp"

using hft::common::align_up;
using hft::common::is_power_of_two;
using hft::common::next_power_of_two;

// Anatomy: TEST(SuiteName, CaseName) { ... }. Each case is independent.
// EXPECT_* records a failure and keeps going (you see all failures in the case);
// ASSERT_* would stop the case immediately (use it when continuing makes no sense).

TEST(BitsTest, IsPowerOfTwo_TrueCases) {
    EXPECT_TRUE(is_power_of_two(1u));
    EXPECT_TRUE(is_power_of_two(2u));
    EXPECT_TRUE(is_power_of_two(4u));
    EXPECT_TRUE(is_power_of_two(1024u));
    EXPECT_TRUE(is_power_of_two(std::uint64_t{1} << 63));
}

TEST(BitsTest, IsPowerOfTwo_FalseCases) {
    EXPECT_FALSE(is_power_of_two(0u));  // 0 is deliberately NOT a power of two
    EXPECT_FALSE(is_power_of_two(3u));
    EXPECT_FALSE(is_power_of_two(6u));
    EXPECT_FALSE(is_power_of_two(1000u));
}

// Table-driven: list {input, expected} pairs and loop. Scales to many cases without repetition.
TEST(BitsTest, NextPowerOfTwo_TableDriven) {
    struct Case {
        std::uint64_t in;
        std::uint64_t want;
    };
    constexpr Case kCases[] = {
        {0u, 1u}, {1u, 1u},   {2u, 2u},       {3u, 4u},
        {5u, 8u}, {17u, 32u}, {1024u, 1024u}, {1025u, 2048u},
    };
    for (const auto& c : kCases) {
        // The trailing << ... is only printed if this particular check fails — great for loops.
        EXPECT_EQ(next_power_of_two(c.in), c.want) << "input = " << c.in;
    }
}

// Property-style: instead of fixed expectations, assert an invariant over many inputs.
TEST(BitsTest, NextPowerOfTwo_ResultIsAlwaysAPowerOfTwo) {
    for (std::uint64_t n = 0; n < 5000u; ++n) {
        const std::uint64_t p = next_power_of_two(n);
        EXPECT_TRUE(is_power_of_two(p)) << "n = " << n;
        EXPECT_GE(p, (n == 0u ? std::uint64_t{1} : n)) << "n = " << n;
    }
}

TEST(BitsTest, AlignUp) {
    EXPECT_EQ(align_up(0u, 64u), 0u);
    EXPECT_EQ(align_up(1u, 64u), 64u);
    EXPECT_EQ(align_up(64u, 64u), 64u);
    EXPECT_EQ(align_up(65u, 64u), 128u);
    EXPECT_EQ(align_up(100u, 8u), 104u);
}

// Because the helpers are constexpr, they also hold at compile time: if any of these were wrong,
// this file would fail to COMPILE (a second safety net, evaluated before the tests even run).
static_assert(is_power_of_two(64u));
static_assert(next_power_of_two(100u) == 128u);
static_assert(align_up(65u, 64u) == 128u);
