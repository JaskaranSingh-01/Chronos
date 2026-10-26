#include "chronos/server/candles.hpp"

#include <algorithm>

namespace simulator::server {

void CandleBuilder::add(const Trade& trade) noexcept
{
    constexpr Timestamp second = 1'000'000'000;
    const Timestamp start = trade.timestamp - trade.timestamp % second;
    if (open_ && start != current_.start) {
        closed_.push_back(current_);
        open_ = false;
    }
    if (!open_) {
        current_ =
            Candle{.start = start, .open = trade.price, .high = trade.price, .low = trade.price, .close = trade.price};
        open_ = true;
    }
    current_.high = std::max(current_.high, trade.price);
    current_.low = std::min(current_.low, trade.price);
    current_.close = trade.price;
    current_.volume += trade.quantity;
}

bool CandleBuilder::current(Candle& bar) const noexcept
{
    if (open_) {
        bar = current_;
    }
    return open_;
}

}  // namespace simulator::server
