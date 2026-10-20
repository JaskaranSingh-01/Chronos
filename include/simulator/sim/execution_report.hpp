#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/types/enums.hpp"
#include "simulator/types/order_id.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/quantity.hpp"
#include "simulator/types/timestamp.hpp"

namespace simulator {

// Constant one-way delays between the strategy and the simulated exchange, in ns.
// order_latency also covers the market-data delay.
struct LatencyConfig
{
    Timestamp order_latency{0};
    Timestamp report_latency{0};
};

enum class TimeInForce : std::uint8_t {
    Ioc,  // fills what crosses now, expires the rest
    Day   // post-only: rests until filled or cancelled; rejected if it would cross
};

// One of our orders. Its id is our own sequence and never reaches MarketBook.
struct OrderRequest
{
    OrderId id{0};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{0};
    TimeInForce time_in_force{TimeInForce::Ioc};
};

enum class ReportType : std::uint8_t {
    Accepted,
    Rejected,
    Fill,
    Cancelled,
    CancelRejected,
    Expired
};

enum class RejectReason : std::uint8_t {
    None,
    WouldCross,   // a post-only order would take liquidity
    InvalidOrder, // zero quantity
    NotTrading    // the symbol is halted, paused or quotation only
};

enum class Liquidity : std::uint8_t {
    None,
    Maker,
    Taker
};

// The fill rule and historical message behind a resting-order fill.
// An Execute with a price is ITCH C, without one ITCH E.
enum class FillCause : std::uint8_t {
    RuleAExecute,
    RuleAExecuteWithPrice,
    RuleDExecute,
    RuleDExecuteWithPrice,
    RuleCAdd,
    RuleCReplace,
    None  // not a maker fill; past fill_cause_count, so it never indexes FillAttribution
};
inline constexpr std::size_t fill_cause_count = 6;
static_assert(static_cast<std::size_t>(FillCause::None) == fill_cause_count);

struct ExecutionReport
{
    Timestamp exchange_time{0};  // at the simulated exchange
    OrderId order_id{0};
    Price price{};               // Fill: execution price
    Quantity quantity{0};        // Fill: filled; Cancelled/Expired/Rejected: removed; Accepted: order quantity
    Quantity leaves{0};          // open quantity after this report
    ReportType type{ReportType::Accepted};
    RejectReason reason{RejectReason::None};
    Liquidity liquidity{Liquidity::None};
    Side side{Side::Buy};
    FillCause cause{FillCause::None};       // maker fill: the rule and ITCH message (E or C) that filled it
    std::uint64_t entry_ahead_quantity{0};  // maker fill: historical shares ahead of the order when it arrived

    bool operator==(const ExecutionReport&) const = default;
};
static_assert(sizeof(ExecutionReport) == 56);

// Portfolio::reserve's pre-trade verdict; checks run in this order.
enum class RiskCheck : std::uint8_t {
    Accepted,
    InvalidOrder,
    OrderTooLarge,
    PositionLimit,
    TooManyOpenOrders,
    Closed  // OrderGateway::send while the gateway is closed, before any check
};

} // namespace simulator
