#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"

namespace simulator::server {

// A historical trade print; `side` is the resting order's, so the aggressor took the other side.
struct Trade
{
    Timestamp timestamp{0};
    Price price{};
    std::uint64_t quantity{0};
    Side side{Side::Buy};

    bool operator==(const Trade&) const = default;
};

// The trade a pending `event` prints (ITCH E at the resting order's price, C at its own, P); call before step().
bool market_trade(const MarketEvent& event, const MarketBook& book, Trade& trade) noexcept;

// A 1 s OHLCV bar of market trades.
struct Candle
{
    Timestamp start{0};  // epoch ns of the second
    Price open{};
    Price high{};
    Price low{};
    Price close{};
    std::uint64_t volume{0};

    bool operator==(const Candle&) const = default;
};

// 1 s bars from market trades only, keeping the whole day's; a second without a trade has no bar.
class CandleBuilder
{
private:
    std::vector<Candle> closed_;
    Candle current_{};
    bool open_{false};

public:
    static constexpr std::size_t day_seconds = 86'400;

    // ponytail: capacity is one session day; a longer stream reallocates in add(). Reset per day if sessions span days.
    CandleBuilder()
    {
        closed_.reserve(day_seconds);
    }

    // Precondition: trades arrive in time order.
    void add(const Trade& trade) noexcept;

    // Closed bars, oldest first.
    std::span<const Candle> closed() const noexcept
    {
        return closed_;
    }

    // The bar of the latest trade's second; false before the first trade.
    bool current(Candle& bar) const noexcept;
};

}  // namespace simulator::server
