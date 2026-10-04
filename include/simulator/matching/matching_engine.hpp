#pragma once

#include <cstddef>
#include <vector>

#include "simulator/matching/trade.hpp"
#include "simulator/orders/order_book.hpp"

namespace simulator {

class MatchingEngine
{
private:
    OrderBook& book_;

public:
    explicit MatchingEngine(OrderBook& book) noexcept
        : book_(book)
    {}

    std::size_t submit(
        Order& order,
        std::vector<Trade>& trades,
        std::vector<Order*>& filled_orders
    );
};

} // namespace simulator