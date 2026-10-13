#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/order_id.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/symbol_id.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {

// Source-agnostic L3 event type. Fields a type does not use are zero.
enum class MarketEventType : std::uint8_t {
    // New order at the back of its price level. ITCH A, F.
    Add,

    // Same id, new price and/or total size, side inherited. Loses priority
    // unless the size only shrinks.
    Modify,

    // new_order_id replaces order_id at a new price and size, side inherited;
    // always loses priority. ITCH U.
    Replace,

    // Reduces order_id by `quantity`; reaching zero removes it. ITCH X.
    Cancel,

    // Removes order_id. ITCH D.
    Delete,

    // order_id traded `quantity`; reduces like Cancel. price is the execution
    // price if the source has one (ITCH C), else 0; the book ignores it. ITCH E, C.
    Execute,

    // Removes every order of `symbol`.
    Clear,

    // Execution not tied to a visible order; the book ignores it. order_id is
    // the source's id or 0, side is informational. ITCH P.
    Trade,

    // The symbol's trading_state changed; no order changes. ITCH H.
    Status
};

inline constexpr std::size_t market_event_type_count = 9;

static_assert(static_cast<std::size_t>(MarketEventType::Status) + 1 == market_event_type_count);

// A symbol's trading state, carried by Status. Anything but Trading means no
// matching, so the book may cross.
enum class TradingState : std::uint8_t {
    Trading,       // zero, so a source without Status events always trades; ITCH 'T'
    Halted,        // ITCH 'H'
    Paused,        // ITCH 'P' (LULD)
    QuotationOnly  // ITCH 'Q'
};

// order_id is unique within one symbol while the order is live.
struct MarketEvent
{
    Timestamp timestamp{0};
    OrderId order_id{0};         // Replace: the original order
    OrderId new_order_id{0};     // Replace only
    Price price{};               // ticks at the instrument's price_scale
    Quantity quantity{0};        // at the instrument's quantity_scale
    SymbolId symbol{0};
    MarketEventType type{MarketEventType::Add};
    Side side{Side::Buy};        // Add (informational on Trade); Modify and Replace inherit it
    TradingState trading_state{TradingState::Trading};  // Status only
};

// 47 bytes of fields, no inner padding.
static_assert(sizeof(MarketEvent) == 48);

} // namespace simulator
