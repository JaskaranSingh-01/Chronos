#pragma once

#include <cstdint>

#include "simulator/market/market_event.hpp"

namespace simulator::server::test {

inline constexpr SymbolId kSymbol = 0;
inline constexpr Timestamp kMs = 1'000'000;
inline constexpr Timestamp kSecond = 1'000'000'000;

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

}  // namespace simulator::server::test
