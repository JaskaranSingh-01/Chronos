#pragma once

#include <cstdint>

#include "chronos/server/generation.hpp"
#include "simulator/market/market_event.hpp"

namespace simulator::server::test {

inline constexpr SymbolId kSymbol = 0;
inline constexpr Timestamp kMs = 1'000'000;
inline constexpr Timestamp kSecond = 1'000'000'000;
inline constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

inline MarketEvent add(Timestamp ts, OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Add,
                       .side = side};
}

// An Execute of `quantity` from order `id`: ITCH E, or ITCH C with its own `price`.
inline MarketEvent execute(Timestamp ts, OrderId id, std::uint64_t quantity, std::int64_t price = 0)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Execute};
}

// Zero fees, 16,384 book slots, and risk limits of position 1,000, order 500 and 4 open orders.
inline SessionConfig config(LatencyConfig latency = {})
{
    return SessionConfig{.session_id = "test",
                         .instrument = kEquity,
                         .symbol = kSymbol,
                         .book_capacity = 16'384,
                         .latency = latency,
                         .limits = {.max_position = 1'000, .max_order_quantity = Quantity{500}, .max_open_orders = 4}};
}

}  // namespace simulator::server::test
