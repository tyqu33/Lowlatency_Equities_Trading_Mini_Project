// libs/common/include/hft/common/bits.hpp
// Responsibility: small, pure bit/size helpers (power-of-two checks, rounding, alignment).
//
// Header-only constexpr utilities. No business logic, no platform deps. Used e.g. to size the
// SPSC ring (capacity must be a power of two so `index & (cap - 1)` works as a fast modulo) and to
// cache-line-align slots. Kept here as a simple, heavily-testable building block.
#pragma once

#include <cstdint>

namespace hft::common {

// True iff x is a power of two. Note: 0 is deliberately NOT a power of two.
constexpr bool is_power_of_two(std::uint64_t x) noexcept {
    return x != 0 && (x & (x - 1)) == 0;
}

// Smallest power of two >= x. Returns 1 for x <= 1.
// (Inputs above 2^63 would overflow; treating that as out of scope for now.)
constexpr std::uint64_t next_power_of_two(std::uint64_t x) noexcept {
    if (x <= 1) {
        return 1;
    }
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}

// Round x up to the next multiple of `alignment`. `alignment` must be a power of two.
constexpr std::uint64_t align_up(std::uint64_t x, std::uint64_t alignment) noexcept {
    return (x + (alignment - 1)) & ~(alignment - 1);
}

}  // namespace hft::common
