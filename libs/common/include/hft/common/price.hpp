// libs/common/include/hft/common/price.hpp
// Responsibility: fixed-point Price value type (int64 scaled by 1/10000 USD) + tick helpers.
//
// Design (see docs/adr/0004-fixed-point-price-and-symbol-interning.md):
//   - Prices are stored as a signed 64-bit integer count of "ticks" where 1 tick = 1/10000 USD
//     (4 implied decimals). So $150.25 is stored as 1'502'500.
//   - Price is a distinct *type* (a class wrapping the int64), NOT a bare `typedef int64_t Price;`.
//     The whole point is that the compiler stops you from mixing a raw int, a scaled price, a share
//     count, or a dollar amount by accident — a class of bug that plagues procedural C code.
//   - It is a small, copyable, constexpr-friendly value type: pass it by value, no heap, no virtuals.
//
// WHERE BODIES LIVE — and why it is not a free choice:
//   A `constexpr` function is implicitly `inline`, and an inline function must be DEFINED in every
//   translation unit that uses it. Put a constexpr body in price.cpp and any other .cpp that calls it
//   fails to link — and it can never be used in a constant expression from outside price.cpp. So:
//     - constexpr members  -> body MUST be here in the header.
//     - non-constexpr ones -> body goes in price.cpp.
//   Which is why the bodies are split as:
//     header : from_ticks / to_dollars / offset_ticks
//     header : is_on_tick / round_down_to_tick / round_up_to_tick
//     .cpp   : from_dollars / from_string / to_string

#pragma once

#include <cstdint>  // std::int64_t
#include <string>   // std::string
#include <string_view>

namespace hft::common {

// Scale factor: how many integer ticks make up $1.00.  $1.00 -> 10'000 ticks.
// `inline constexpr` at namespace scope = a compile-time constant that is safe to define in a header
// included by many .cpp files (no duplicate-symbol problem). (constexpr = known at compile time.)
// The digit separators (') are just readability; 10'000 == 10000.
inline constexpr std::int64_t kPriceScale = 10'000;

class Price {
public:
    // ---- Construction ------------------------------------------------------

    // Default: a zero price. `= default` tells the compiler to generate the trivial version.
    // constexpr + noexcept: usable at compile time, promises never to throw.
    constexpr Price() noexcept = default;

    // Build directly from a raw tick count (the stored representation).
    // This is a *named constructor* (static factory) so call sites read clearly:
    //   Price::from_ticks(1'502'500)  vs  Price::from_dollars(150.25)  are unambiguous.
    static constexpr Price from_ticks(std::int64_t ticks) noexcept {
        Price p;
        p.ticks_ = ticks;
        return p;
    }

    // Build from a floating-point dollar amount, e.g. 150.25 -> 1'502'500 ticks.
    // Use ONLY at input boundaries (parsing config / user input); never on the hot path, because
    // double has rounding error. Should round to the nearest tick.
    static Price from_dollars(double dollars) noexcept;

    // Parse from a decimal string like "150.25" or "0.0001".
    // Returns true on success and writes the result into `out`; returns false on malformed input.
    // (No exceptions on the parse path — failure is reported via the bool, HFT-style.)
    static bool from_string(std::string_view text, Price& out) noexcept;

    // ---- Accessors ---------------------------------------------------------

    // The raw stored integer (tick count). This is what the order book compares / indexes on.
    constexpr std::int64_t ticks() const noexcept { return ticks_; }

    // Convert to a double dollar amount. DISPLAY / logging only — never for comparisons or math.
    constexpr double to_dollars() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(kPriceScale);
    }

    // Human-readable string, e.g. "150.2500". For logs / UI, not the hot path.
    std::string to_string() const;

    // ---- Comparison (most-used part: price-time priority lives on these) ----
    // Compare the underlying ticks. Defined inline + constexpr because they must be trivial/fast.
    // (`friend` lets these free-function operators see the private ticks_. In C++20 you could
    //  instead write a single `operator<=>` and `operator==` to synthesize all six; spelled out
    //  here so every one is visible while you're learning.)
    friend constexpr bool operator==(Price a, Price b) noexcept { return a.ticks_ == b.ticks_; }
    friend constexpr bool operator!=(Price a, Price b) noexcept { return a.ticks_ != b.ticks_; }
    friend constexpr bool operator<(Price a, Price b) noexcept { return a.ticks_ < b.ticks_; }
    friend constexpr bool operator<=(Price a, Price b) noexcept { return a.ticks_ <= b.ticks_; }
    friend constexpr bool operator>(Price a, Price b) noexcept { return a.ticks_ > b.ticks_; }
    friend constexpr bool operator>=(Price a, Price b) noexcept { return a.ticks_ >= b.ticks_; }

    // ---- Limited, meaningful arithmetic ------------------------------------
    // We deliberately do NOT provide Price + Price (adding two prices is meaningless) or Price*Price.
    // We DO provide moving a price by a signed number of ticks, and the difference between two
    // prices (a signed spread, in ticks).

    // Shift this price up / down by `n` ticks (n may be negative). Returns a new Price.
    constexpr Price offset_ticks(std::int64_t n) const noexcept { return from_ticks(ticks_ + n); }

    // Difference between two prices, in ticks (can be negative). e.g. ask - bid = spread.
    friend constexpr std::int64_t operator-(Price a, Price b) noexcept { return a.ticks_ - b.ticks_; }

    // ---- Tick-size helpers (Rule 612 min price increment) ------------------
    // `tick_size` is itself expressed in ticks (e.g. a $0.01 min increment = 100 ticks).

    // Is this price an exact multiple of tick_size? (i.e. a legal price for that instrument)
    //
    // Expressed via the rounding helpers rather than a separate `% == 0`: a value sits on a tick
    // exactly when flooring and ceiling it agree. One characterisation, one place to be wrong.
    // (Costs two divisions where a modulo would do one — revisit if this ever lands on a hot path.)
    constexpr bool is_on_tick(std::int64_t tick_size) const noexcept {
        return round_down_to_tick(tick_size) == round_up_to_tick(tick_size);
    }

    // Round this price DOWN / UP to the nearest multiple of tick_size — toward -infinity and
    // +infinity respectively.
    //
    //   THE TRAP — C++ integer division truncates toward ZERO, not toward -infinity:
    //       -150 / 100  ==  -1      (so -1 * 100 == -100)
    //   but rounding -150 DOWN to a multiple of 100 must give -200, not -100.
    //   The naive `(ticks_ / tick_size) * tick_size` is therefore correct for positive prices and
    //   silently wrong for negative ones. Prices here are signed on purpose (spreads, offsets), so
    //   this case is reachable. The correction is applied only when the division leaves a remainder:
    //   an exact division has thrown nothing away, so nudging it would move a full tick too far.
    //
    //   Behaviour (tick_size = 100):
    //       150 -> down 100, up 200        -150 -> down -200, up -100
    //       200 -> down 200, up 200        -200 -> down -200, up -200   (already on tick: no move)
    //
    //   Precondition: tick_size > 0
    constexpr Price round_down_to_tick(std::int64_t tick_size) const noexcept {
        if(tick_size <= 0) return {};
        else if(ticks_ == 0) return *this;
        else if(ticks_ > 0 || ticks_ % tick_size == 0) return from_ticks((ticks_ / tick_size) * tick_size);
        // when <0, subtract one unit to towards -infinity
        return from_ticks(((ticks_ - tick_size) / tick_size) * tick_size);
    }
    constexpr Price round_up_to_tick(std::int64_t tick_size) const noexcept {
        if(tick_size <= 0) return {};
        else if(ticks_ == 0) return *this;
        else if(ticks_ < 0 || ticks_ % tick_size == 0) return from_ticks((ticks_ / tick_size) * tick_size);
        // when >0, add one unit to towards infinity
        return from_ticks(((ticks_ + tick_size) / tick_size) * tick_size);
    }

    // ---- Named constants ---------------------------------------------------
    static constexpr Price zero() noexcept { return Price{}; }

private:
    // The single data member: signed so price *differences* can be negative and to avoid
    // unsigned-underflow surprises. Initialized to 0.
    std::int64_t ticks_ = 0;
};

// Sanity: a Price must be as cheap as a raw int64 (no hidden overhead). If this fails, something
// (a vtable, padding, an extra member) crept in.
static_assert(sizeof(Price) == sizeof(std::int64_t));

}  // namespace hft::common
