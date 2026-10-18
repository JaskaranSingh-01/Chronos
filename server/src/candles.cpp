#include "chronos/server/candles.hpp"

#include <algorithm>

#include "simulator/orders/order.hpp"

namespace simulator::server {

bool market_trade(const MarketEvent& event, const MarketBook& book, Trade& trade) noexcept
{
    if (event.symbol != book.symbol()) {
        return false;
    }
    if (event.type == MarketEventType::Trade) {
        trade = Trade{
            .timestamp = event.timestamp, .price = event.price, .quantity = event.quantity.value(), .side = event.side};
        return true;
    }
    if (event.type != MarketEventType::Execute) {
        return false;
    }
    const Order* order = book.find(event.order_id);
    if (order == nullptr || event.quantity.is_zero() || event.quantity > order->remaining_quantity()) {
        return false;
    }
    trade = Trade{.timestamp = event.timestamp,
                  .price = event.price.ticks != 0 ? event.price : order->price(),
                  .quantity = event.quantity.value(),
                  .side = order->side()};
    return true;
}

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
