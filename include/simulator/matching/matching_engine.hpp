#pragma once

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

    std::vector<Trade> submit(Order& order);
};

} // namespace simulator