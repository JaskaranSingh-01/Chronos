#pragma once

#include <cstddef>
#include <vector>

#include "simulator/matching/matching_engine.hpp"
#include "simulator/orders/order_pool.hpp"
#include "simulator/clock/clock.hpp"

namespace simulator {

class Simulator
{
private:
    Clock clock_;

    OrderPool order_pool_;
    OrderBook order_book_;
    MatchingEngine matching_engine_;

    std::vector<Trade> trades_;

public:
    explicit Simulator(
        std::size_t order_capacity
    );

    Order* submit_order(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    );
    bool cancel_order(OrderId id) noexcept;
    void advance_to(Timestamp timestamp) noexcept;
    bool release_order(Order& order) noexcept;

    const std::vector<Trade>& trades() const noexcept;

    const Clock& clock() const noexcept;
    const OrderBook& order_book() const noexcept;

    std::size_t orders_in_use() const noexcept;

    std::size_t orders_available() const noexcept;
};

} // namespace simulator