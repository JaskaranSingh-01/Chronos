#include "simulator/simulator.hpp"

#include <algorithm>

namespace simulator {

Simulator::Simulator(std::size_t order_capacity)
    : clock_{},
      order_pool_{order_capacity},
      order_book_{},
      matching_engine_{order_book_}
{
    // Initial reusable trade-buffer capacity.
    // This avoids allocations for normal small matches.
    trades_.reserve(64);
    filled_orders_.reserve(64);

}

Order* Simulator::submit_order(
    OrderId id,
    Side side,
    Price price,
    Quantity quantity,
    Timestamp timestamp
)
{
    Order* order = order_pool_.acquire(
        id,
        side,
        price,
        quantity,
        timestamp
    );

    if (order == nullptr) {
        return nullptr;
    }

    if (!order->activate()) {
        return nullptr;
    }

    matching_engine_.submit(
        *order,
        trades_,
        filled_orders_
    );

    return order;
}

std::size_t Simulator::release_filled_orders() noexcept
{
    std::size_t released = 0;

    for (Order* order : filled_orders_)
    {
        if (order == nullptr) {
            continue;
        }

        if (order->state() != OrderState::Filled) {
            continue;
        }

        if (order_book_.find(order->id()) != nullptr) {
            continue;
        }

        if (order_pool_.release(*order)) {
            ++released;
        }
    }

    filled_orders_.clear();

    return released;
}

void Simulator::advance_to(Timestamp timestamp) noexcept
{
    clock_.advance_to(timestamp);
}

bool Simulator::cancel_order(OrderId id) noexcept
{
    Order* order = order_book_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order_book_.cancel(id)) {
        return false;
    }

    return order_pool_.release(*order);
}

bool Simulator::release_order(Order& order) noexcept
{
    if (order.state() != OrderState::Filled &&
        order.state() != OrderState::Cancelled)
    {
        return false;
    }

    if (order_book_.find(order.id()) != nullptr)
    {
        return false;
    }

    if (!order_pool_.release(order)) {
        return false;
    }

    std::erase(filled_orders_, &order);

    return true;
}

const std::vector<Trade>& Simulator::trades() const noexcept {
    return trades_;
}

const Clock& Simulator::clock() const noexcept {
    return clock_;
}

const OrderBook& Simulator::order_book() const noexcept {
    return order_book_;
}

std::size_t Simulator::orders_in_use() const noexcept {
    return order_pool_.in_use();
}

std::size_t Simulator::orders_available() const noexcept {
    return order_pool_.available();
}

} // namespace simulator
