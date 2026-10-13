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

// The source-agnostic L3 event vocabulary (v2). Fields an enumerator does not
// name are zero. Native message types per source:
//
//   Action    ITCH   LOBSTER   Databento MBO
//   Add       A, F   1         A
//   Modify    -      -         M
//   Replace   U      -         -
//   Cancel    X      2         C
//   Delete    D      3         C
//   Execute   E, C   4         F
//   Clear     -      -         R
//   Trade     P      5         T
//   Status    H      7         status (StatusMsg)
//
// Not mapped (the source skips and counts them): ITCH S, Q, B, h (operational
// halt, one market centre) and the administrative types (R feeds
// InstrumentRegistry); LOBSTER 6 (cross trade).
enum class MarketEventType : std::uint8_t {
    // A new visible order. order_id = new id, side, price = limit, quantity = size.
    // Book: joins the back of its price level.
    Add,

    // Same id, new price and/or size. order_id = existing, price = new price,
    // quantity = new total size; side is inherited.
    // Book: a price change or size increase loses priority (back of the queue);
    // a size decrease alone keeps it. Defined now, applied once a source emits it.
    Modify,

    // A new id replaces the old order. order_id = old, new_order_id = new,
    // price = new price, quantity = new size; side is inherited.
    // Book: always loses priority.
    Replace,

    // Partial reduce. order_id = existing, quantity = size removed.
    // Book: reduce; quantity == remaining removes the order.
    Cancel,

    // Full removal. order_id = existing.
    // Book: remove the order.
    Delete,

    // A visible order traded. order_id = existing, quantity = size executed,
    // price = execution price if the source knows it (ITCH C), else 0.
    // Book: reduce; quantity == remaining removes the order. The book ignores
    // price; the fill model uses it (ExecutionSimulator rules a, d).
    Execute,

    // Drop the whole book of `symbol`. Every other field is zero.
    // Book: remove every order. Defined now, applied once a source emits it.
    Clear,

    // An execution not tied to a visible order. order_id = source id or 0,
    // price = trade price, quantity = size; side is informational, per adapter.
    // Book: none; the book ignores it.
    Trade,

    // The symbol's trading state changed. trading_state = the new state; every
    // other field but timestamp and symbol is zero.
    // Book: records it (MarketBook::trading_state()); no order changes.
    Status
};

inline constexpr std::size_t market_event_type_count = 9;

static_assert(static_cast<std::size_t>(MarketEventType::Status) + 1 == market_event_type_count);

// A symbol's trading state, carried by Status. Anything but Trading means no
// matching: the venue queues orders without executing them, so the book may cross.
//   State          ITCH H state   LOBSTER 7 price   Databento status action
//   Trading        'T'            1                 Trading
//   Halted         'H'            -1                Halt, Suspend, NotAvailableForTrading
//   Paused         'P' (LULD)     -                 Pause
//   QuotationOnly  'Q'            0                 Quoting
// LOBSTER 7 with any other price is SourceError::InvalidTradingState, as is an
// unknown ITCH state byte. ITCH also halts a security left out of the pre-open
// trading action spin (ItchSource). Databento's other StatusAction values (dbn:
// None, PreOpen, PreCross, Cross, Rotation, NewPriceIndication, PreClose,
// Close, PostClose, SsrChange) are session phases or flags and emit no Status.
enum class TradingState : std::uint8_t {
    Trading,  // zero: a source without Status events always trades
    Halted,
    Paused,
    QuotationOnly
};

// Plain data, no behaviour. order_id is unique within one symbol while the
// order is live.
struct MarketEvent
{
    Timestamp timestamp{0};      // int64 ns since the Unix epoch, UTC
    OrderId order_id{0};         // Replace: the original order
    OrderId new_order_id{0};     // Replace only
    Price price{};               // ticks at the instrument's price_scale
    Quantity quantity{0};        // uint64 at the instrument's quantity_scale
    SymbolId symbol{0};
    MarketEventType type{MarketEventType::Add};
    Side side{Side::Buy};        // Add (informational on Trade); Modify and Replace inherit it
    TradingState trading_state{TradingState::Trading};  // Status only
};

// 47 bytes of fields with no inner padding, rounded up to 48.
static_assert(sizeof(MarketEvent) == 48);

} // namespace simulator
