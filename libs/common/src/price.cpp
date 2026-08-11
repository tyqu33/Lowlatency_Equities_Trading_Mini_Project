// libs/common/src/price.cpp
// Responsibility: the NON-constexpr parts of Price — parsing and formatting.
//
// Only three functions live here. Everything else is constexpr and therefore had to be defined
// inline in price.hpp (see the note at the top of that header for why).

#include "hft/common/price.hpp"

#include <cmath>    // std::llround
#include <cstdlib>  // std::abs

namespace hft::common {

// Build from a floating-point dollar amount: 150.25 -> 1'502'500 ticks.
//
// Notes:
//   - Round to the NEAREST tick, don't truncate. 150.25 * 10'000 may evaluate to 1502499.9999...
//     in binary floating point, so a plain cast to int64 would give 1502499 — off by one tick.
//     std::llround is the tool.
//   - Negative dollars must work (-1.5 -> -15'000).
//   - Boundary use only (config parsing / test setup). Never on the hot path.
Price Price::from_dollars(double dollars) noexcept {
    return from_ticks(std::llround(dollars * kPriceScale));
}

// Parse a decimal string: "150.25" -> 1'502'500. Returns false (and leaves `out` untouched) on
// malformed input. No exceptions — failure is the bool.
//
// Cases the tests cover:
//   accept: "150.25"  "150"  "150.2500"  "0.0001"  "-1.5"  "+2.5"  "0"
//   reject: ""  "abc"  "1.2.3"  "1.00001" (5 decimals — more precision than a tick)  "."  "-"
// Notes:
//   - kPriceScale is 10'000, i.e. exactly 4 implied decimals. "150.2" means 150.2000, NOT 150.0002 —
//     you must LEFT-pad the fraction to 4 digits, which is the easiest place to get this wrong.
//   - Do the arithmetic in integers. Do NOT parse to double and call from_dollars — that reintroduces
//     the rounding error this whole type exists to avoid.
//   - Watch the sign: "-0.0001" is -1 tick, and the '-' is lost if you parse the two halves
//     independently and only apply the sign to the integer part.

// 10^0 .. 10^4 — the only exponents a 4-implied-decimal parser can ever need. A table beats
// std::pow here on all three counts that matter: exact integer math (no float in a type that exists
// to avoid float), no function call, and no -Wfloat-conversion warning.
constexpr std::int64_t kPow10[] = {1, 10, 100, 1'000, 10'000};

bool Price::from_string(std::string_view text, Price& out) noexcept {
    if(text == "") return false;
    bool has_dot = false;
    int count_sign = 0, dot_count = 0, post_dot_count = -1;
    bool ifNegative = false;
    long long integral_part = 0, decimal_part = 0;

    for(int i=0; i < text.length(); i++){
        if((text[i] < '0' || text[i] > '9') && text[i] != '+' && text[i] != '-' && text[i] != '.') return false;
        if(i != 0 && (text[i] == '+' || text[i] == '-')) return false;
        if(text[i] == '+' || text[i] == '-') count_sign++;
        else if(text[i] == '.') dot_count++;
        if(i == 0 && text[i] == '-') ifNegative = true;

        if(text[i] == '+' || text[i] == '-') continue;
        if(text[i] == '.' && post_dot_count == -1){has_dot = true;} 
        if(has_dot) post_dot_count++;
        if(!has_dot) integral_part = integral_part * 10 + text[i] - '0';
        else if(has_dot && text[i] != '.') decimal_part = decimal_part * 10 + text[i] - '0';
    }
    if(count_sign > 1 || dot_count > 1) return false;
    else if(text.length() == 1 && (count_sign == 1 || dot_count == 1)) return false;
    if(post_dot_count >= 5) return false;
    if(has_dot) decimal_part = decimal_part * kPow10[4 - post_dot_count];

    std::int64_t temp = (std::int64_t)(integral_part * kPriceScale + decimal_part);
    out.ticks_ = !ifNegative ? temp : (-1) * temp;

    return true;
}

// Render as a fixed 4-decimal string: 1'502'500 -> "150.2500".
//
// Notes:
//   - Always exactly 4 decimals, zero-padded: 1 tick -> "0.0001", 10'000 -> "1.0000".
//   - Negative: -1 tick -> "-0.0001". Note the integer part is 0, so the sign cannot come from
//     formatting `ticks_ / kPriceScale` alone — handle it explicitly.
//   - Logging / UI only. Not the hot path, so clarity beats cleverness here.
std::string Price::to_string() const {
    std::int64_t n = ticks();
    std::string str = std::to_string(n);
    bool ifNegative = (str[0] == '-');
    if (ifNegative) str.erase(0, 1);

    std::string integral_part = "", decimal_part = "";
    if(str.length() >= 5){
        integral_part = str.substr(0, str.length()-4);
        decimal_part = str.substr(str.length()-4, 4);
    } else {
        decimal_part = str;
    }
    while(decimal_part.length() < 4){
        decimal_part = "0" + decimal_part;
    }
    if(integral_part == "") integral_part = "0";
    return (ifNegative ? "-" : "") + integral_part + "." + decimal_part;
}

}  // namespace hft::common
