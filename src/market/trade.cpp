#include "simulator/market/trade.hpp"

#include "simulator/orders/order.hpp"

namespace simulator {

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

} // namespace simulator
