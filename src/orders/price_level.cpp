#include "simulator/orders/price_level.hpp"

namespace simulator {

bool PriceLevel::add(Order& order) noexcept
{
    if (order.price() != price_) {
        return false;
    }

    if (order.level_ != nullptr) {
        return false;
    }

    order.prev_ = tail_;
    order.next_ = nullptr;
    order.level_ = this;

    if (tail_ != nullptr) {
        tail_->next_ = &order;
    } else {
        head_ = &order;
    }

    tail_ = &order;

    ++order_count_;
    total_quantity_ += order.remaining_quantity_.value();

    return true;
}

bool PriceLevel::remove(Order& order) noexcept
{
    if (order.level_ != this) {
        return false;
    }

    if (order.prev_ != nullptr) {
        order.prev_->next_ = order.next_;
    } else {
        head_ = order.next_;
    }

    if (order.next_ != nullptr) {
        order.next_->prev_ = order.prev_;
    } else {
        tail_ = order.prev_;
    }

    order.prev_ = nullptr;
    order.next_ = nullptr;
    order.level_ = nullptr;

    --order_count_;
    // Callers unlink before delete_order() zeroes the remaining quantity.
    total_quantity_ -= order.remaining_quantity_.value();

    return true;
}

} // namespace simulator