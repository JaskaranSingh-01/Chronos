#pragma once

#include <cstdint>
#include <string_view>

#include "chronos/server/session_strategy.hpp"
#include "simulator/sim/execution_report.hpp"

namespace simulator::server {

// Why the session refused a command or control, or why the simulator rejected an order or cancel.
enum class ErrorCode : std::uint8_t {
    None,
    QueueFull,
    NotReady,  // seeking or ended, or a manual order while a strategy is attached
    StaleGeneration,
    BadRequest,
    LogUnavailable,
    NoLiquidity,  // a market order with no opposite price to band around
    RiskInvalidOrder,
    RiskOrderTooLarge,
    RiskPositionLimit,
    RiskTooManyOpenOrders,
    WouldCross,
    NotTrading,
    InvalidOrder,
    CancelRejected  // the order was no longer resting
};

// The protocol's name for `code`, e.g. "queue_full".
constexpr std::string_view to_string(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::None:
        return "none";
    case ErrorCode::QueueFull:
        return "queue_full";
    case ErrorCode::NotReady:
        return "not_ready";
    case ErrorCode::StaleGeneration:
        return "stale_generation";
    case ErrorCode::BadRequest:
        return "bad_request";
    case ErrorCode::LogUnavailable:
        return "log_unavailable";
    case ErrorCode::NoLiquidity:
        return "no_liquidity";
    case ErrorCode::RiskInvalidOrder:
        return "risk_invalid_order";
    case ErrorCode::RiskOrderTooLarge:
        return "risk_order_too_large";
    case ErrorCode::RiskPositionLimit:
        return "risk_position_limit";
    case ErrorCode::RiskTooManyOpenOrders:
        return "risk_too_many_open_orders";
    case ErrorCode::WouldCross:
        return "would_cross";
    case ErrorCode::NotTrading:
        return "not_trading";
    case ErrorCode::InvalidOrder:
        return "invalid_order";
    case ErrorCode::CancelRejected:
        return "cancel_rejected";
    }
    return "unknown";
}

enum class CommandType : std::uint8_t {
    Order,
    Cancel,
    Attach,
    Detach
};

enum class OrderType : std::uint8_t {
    PostOnly,  // rests, or is rejected WouldCross
    Ioc,       // takes levels up to its price; the rest expires
    Market     // an Ioc limited to market_band_percent beyond the best opposite price
};

// The market-order protection band, in percent of the best opposite price.
inline constexpr std::int64_t market_band_percent = 1;

// A trading command from the client.
struct Command
{
    std::uint64_t command_seq{0};   // set by ReplaySession::submit
    std::uint64_t generation{0};    // the generation the client sent it to
    Timestamp received_wall_ts{0};  // set by ReplaySession::submit
    CommandType type{CommandType::Order};
    OrderType order_type{OrderType::PostOnly};  // Order
    Side side{Side::Buy};                       // Order
    Price price{};                              // Order: PostOnly and Ioc
    Quantity quantity{0};                       // Order
    std::uint64_t client_id{0};                 // Order: the client's own reference
    OrderId order_id{0};                        // Cancel
    StrategySpec strategy{};                    // Attach

    bool operator==(const Command&) const = default;
};

}  // namespace simulator::server
