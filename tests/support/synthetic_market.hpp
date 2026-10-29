#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "simulator/market/market_event.hpp"

namespace simulator {

// A quoting market around a fixed mid: three levels a side, 6-40 live orders
// of 10-400 shares, partial executions and deletes, timestamps 0-199 ns apart
// (ties included).
inline std::vector<MarketEvent> synthetic_market(std::uint64_t seed, std::size_t count)
{
    struct Live
    {
        OrderId id;
        std::uint64_t remaining;
    };

    std::mt19937_64 rng{seed};
    std::vector<MarketEvent> events;
    std::vector<Live> live;
    Timestamp ts = 1'000;
    OrderId next_id = 1;

    while (events.size() < count) {
        ts += static_cast<Timestamp>(rng() % 200);
        const std::uint64_t roll = rng() % 10;

        if (live.size() < 6 || (roll < 4 && live.size() < 40)) {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            const auto offset = static_cast<std::int64_t>(100 * (1 + rng() % 3));
            const std::uint64_t quantity = 10 * (1 + rng() % 40);
            events.push_back(MarketEvent{.timestamp = ts,
                                         .order_id = next_id,
                                         .price = Price{side == Side::Buy ? 1'000'000 - offset : 1'000'000 + offset},
                                         .quantity = Quantity{quantity},
                                         .type = MarketEventType::Add,
                                         .side = side});
            live.push_back({next_id++, quantity});
            continue;
        }

        const std::size_t pick = rng() % live.size();
        Live& order = live[pick];
        const bool execute = roll < 7;
        const std::uint64_t quantity = execute ? 1 + rng() % order.remaining : order.remaining;
        events.push_back(MarketEvent{.timestamp = ts,
                                     .order_id = order.id,
                                     .quantity = Quantity{quantity},
                                     .type = execute ? MarketEventType::Execute : MarketEventType::Delete});
        order.remaining -= quantity;
        if (order.remaining == 0) {
            live[pick] = live.back();
            live.pop_back();
        }
    }
    return events;
}

} // namespace simulator
