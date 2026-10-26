#pragma once

#include <cstdint>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"

namespace simulator {

// A historical trade print; `side` is the resting order's, so the aggressor took the other side.
struct Trade
{
    Timestamp timestamp{0};
    Price price{};
    std::uint64_t quantity{0};
    Side side{Side::Buy};

    bool operator==(const Trade&) const = default;
};

// The trade `event` prints, read before `book` applies it: ITCH E at the resting order's price, C at its own, P as
// given. False for any other event, and for an execution the book would reject.
bool market_trade(const MarketEvent& event, const MarketBook& book, Trade& trade) noexcept;

} // namespace simulator
