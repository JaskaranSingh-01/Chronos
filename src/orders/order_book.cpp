#include "simulator/orders/order_book.hpp"

namespace simulator {

// Detaches the order from its level and erases the level if it empties.
// Leaves orders_ and the order's state to the caller.
bool OrderBook::unlink(Order& order) noexcept
{
    PriceLevel* level = order.level_;

    if (level == nullptr || !level->remove(order)) {
        return false;
    }

    if (level->empty()) {
        if (order.side() == Side::Buy) {
            bids_.erase(level->price());
        } else {
            asks_.erase(level->price());
        }
    }

    return true;
}

bool OrderBook::add(Order& order) noexcept
{
    if (order.remaining_quantity().is_zero()) {
        return false;
    }

    if (order.state() != OrderState::Active && order.state() != OrderState::PartiallyFilled) {
        return false;
    }

    if (orders_.find(order.id()) != nullptr) {
        return false;
    }

    if (order.side() == Side::Buy) {

        auto [it, inserted] = bids_.try_emplace(order.price(), order.price());

        PriceLevel& level = it->second;

        if (!level.add(order)) {
            if (inserted && level.empty()) {
                bids_.erase(it);
            }

            return false;
        }

    } else {

        auto [it, inserted] = asks_.try_emplace(order.price(), order.price());

        PriceLevel& level = it->second;

        if (!level.add(order)) {
            if (inserted && level.empty()) {
                asks_.erase(it);
            }

            return false;
        }
    }

    orders_.insert(order.id(), &order);

    return true;
}

bool OrderBook::cancel(OrderId id) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (order->state() != OrderState::Active && order->state() != OrderState::PartiallyFilled) {
        return false;
    }

    if (!unlink(*order)) {
        return false;
    }

    if (!order->cancel()) {
        return false;
    }

    orders_.erase(id);

    return true;
}

Order* OrderBook::find(OrderId id) noexcept
{
    return orders_.find(id);
}

const Order* OrderBook::find(OrderId id) const noexcept
{
    return orders_.find(id);
}

const Order* OrderBook::best_bid() const noexcept
{
    if (bids_.empty()) {
        return nullptr;
    }

    return bids_.begin()->second.front();
}

const Order* OrderBook::best_ask() const noexcept
{
    if (asks_.empty()) {
        return nullptr;
    }

    return asks_.begin()->second.front();
}

bool OrderBook::empty() const noexcept
{
    return orders_.size() == 0;
}

bool OrderBook::execute(OrderId id, Quantity quantity) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->execute(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Filled) {
        return true;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

bool OrderBook::cancel(OrderId id, Quantity quantity) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->cancel(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Cancelled) {
        return true;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

bool OrderBook::erase(OrderId id) noexcept
{
    Order* order = orders_.find(id);

    if (order == nullptr) {
        return false;
    }

    if (!unlink(*order)) {
        return false;
    }

    orders_.erase(id);

    return true;
}

} // namespace simulator