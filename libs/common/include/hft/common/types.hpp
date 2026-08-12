// libs/common/include/hft/common/types.hpp
// Responsibility: the core domain vocabulary shared by every component — identifiers, quantities,
// and the four order-attribute enums.
//
// These are DOMAIN types, deliberately independent of any wire format. OUCH and ITCH encode the
// same concepts as single ASCII characters ('B' for buy, 'S' for sell, …); that mapping belongs in
// libs/protocol, not here. Keeping the two apart means a second protocol (FIX in V2) plugs in
// without touching a single line of business logic.
//
// ON `enum class` (as opposed to a plain `enum`):
//   - Scoped: you must write `Side::Buy`, never a bare `Buy`. Two enums can both have a `None`
//     without colliding.
//   - No implicit conversion to int: `if (side == 1)` will not compile, and neither will passing a
//     Side where an OrderType is expected. A plain `enum` allows both, silently.
//   - Explicit underlying type (`: std::uint8_t` below): fixes the size at one byte. That matters
//     here because these fields sit inside fixed-size messages in fixed-size ring slots, where
//     every byte of padding is wasted cache line.
//
// Values are assigned implicitly (0, 1, 2, …). Nothing persists or transmits these numbers, so
// their exact values carry no meaning — only the names do. Reordering them is safe; if that ever
// stops being true (e.g. they get written to a journal), pin them explicitly at that point.

#pragma once

#include <cstdint>

namespace hft::common {

// ============================================================================
// Identifiers
// ============================================================================

// An order carries TWO independent identities, assigned by two different parties. Conflating them
// is a classic source of bugs, so they get separate names even though both are integers.
//
//   1. The PARTICIPANT chooses a token before sending, and uses it to correlate the venue's
//      responses back to its own order records. Unique within that participant only.
//   2. The VENUE assigns a reference number on acceptance. This is the market-wide identity: every
//      later message about that order — executed, cancelled, deleted — refers to it, and the public
//      ITCH feed keys the whole book on it.
//
// (This is the same shape as a Chinese broker's 系统内部委托编号 → 委托确认编号 returned by the
// 转换机: one id you generate, one the market hands back.)

// Participant-assigned, sent with the order. OUCH carries this as 14 alphanumeric bytes on the
// wire; we keep an integer internally and let the protocol layer format it.
using OrderToken = std::uint64_t;

// Venue-assigned on acceptance. The market-wide identity of a resting order, and the key the local
// book is indexed by.
using OrderRefNum = std::uint64_t;

// The account the order belongs to. Pre-trade risk limits (SEC 15c3-5) are aggregated per account,
// so this is read on the critical path.
using AccountId = std::uint32_t;

// Share quantity.
//
// Signed on purpose, for the same reason Price is: quantities get subtracted. A position delta, an
// unfilled remainder, a limit headroom check — all of these can legitimately go negative, and an
// unsigned type would silently wrap to an enormous positive number instead. That is a bug class
// worth spending the sign bit to avoid.
//
// 64-bit, though US equity order sizes fit in 32 easily: cumulative volumes and position
// aggregates are what actually overflow, and having one quantity type avoids conversions at every
// boundary. ITCH puts share counts in a uint32 on the wire — narrow at the protocol layer.
using Qty = std::int64_t;

// ============================================================================
// Order attributes
// ============================================================================

// Which way the order goes.
//
// US equities distinguish a long sale from a short sale ON THE ORDER ITSELF, which a market like
// A-shares does not — there, 融券卖出 is a separate business type rather than a flag. The reason is
// regulatory: SEC Reg SHO requires the broker to mark short sales (Rule 200) and to have located
// borrowable stock before sending one (Rule 203), and Rule 201 restricts short sales entirely once
// a stock falls 10% intraday. All three checks read this field, in RISK, before the order is sent.
//
// Note the venue's public feed does NOT republish the distinction: ITCH's Add Order message carries
// only buy or sell. The exchange knows; the tape does not tell you.
enum class Side : std::uint8_t {
    Buy,              // 买入
    Sell,             // 卖出 — long sale, seller owns the shares
    SellShort,        // 卖空 — seller does not own them; requires a locate
    SellShortExempt,  // 卖空(豁免)— exempt from the Rule 201 price test
};

// How the order is priced.
//
// IMPORTANT: a Market order carries NO price. Do not write code that assumes every order arrives
// with one — the venue decides what a market order pays, at the moment it matches. The same is true
// of the price-referencing order types a Chinese exchange offers (本方最优 / 对手方最优): the price
// is assigned by the matching engine on arrival, not supplied by the sender. A participant
// rebuilding its book from an order feed therefore cannot derive it (see ADR-0007).
enum class OrderType : std::uint8_t {
    Limit,   // 限价 — executes at the stated price or better, rests in the book otherwise
    Market,  // 市价 — executes against whatever is available; no price on the order
};

// How long the order stays alive. Commonly abbreviated TIF(Time in Force).
//
// This dimension barely exists in A-shares, where essentially everything is 当日有效 — which is why
// the concept feels unfamiliar coming from that market. The US treats price type and lifetime as
// two INDEPENDENT fields, and that is the design insight worth carrying over:
//
//     A-share 「最优五档即时成交剩余撤销」  ==  OrderType::Market + TimeInForce::IOC
//
// One market folded the two ideas into a single order type and had to invent a new one for each
// combination; the other kept them orthogonal and gets every combination for free.
enum class TimeInForce : std::uint8_t {
    Day,  // 当日有效 — cancelled automatically at the close
    IOC,  // Immediate Or Cancel — 立即成交,剩余撤销. Never rests in the book
    FOK,  // Fill Or Kill — 全额成交,否则全撤. All at once or nothing
    GTC,  // Good Till Cancel — survives across sessions. No A-share equivalent
};

// The capacity in which the sender is acting — whose money is at risk.
//
// This is NOT a quantity. It answers "are you trading for a customer, or for yourself?", and it
// exists as a per-order field in the US because one broker-dealer routes both kinds of flow through
// the same session. In a Chinese broker this distinction is usually carried by the 席位 or the
// account rather than by the individual order.
//
// RISK reads it: principal and agency flow carry different 15c3-5 limits, and they are reported
// differently to regulators.
enum class Capacity : std::uint8_t {
    Agency,             // 代理 — trading on behalf of a customer
    Principal,          // 自营 — trading the firm's own account, firm bears the risk
    RisklessPrincipal,  // 无风险自营 — bought to fill a customer order, offset immediately
};

}  // namespace hft::common
