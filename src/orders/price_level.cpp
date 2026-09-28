#include "simulator/orders/price_level.hpp"

namespace simulator {

bool PriceLevel::add(Order& order) noexcept {
    // Order must belong to this price level.
    if (order.price() != price_) {
        return false;
    }

    // An order cannot belong to two price levels.
    if (order.level_ != nullptr) {
        return false;
    }

    // Insert at the back of the FIFO list.
    order.prev_ = tail_;
    order.next_ = nullptr;
    order.level_ = this;

    if (tail_ != nullptr) {
        // Existing last order points to the new order.
        tail_->next_ = &order;
    } else {
        // First order in the level.
        head_ = &order;
    }

    tail_ = &order;

    ++order_count_;

    return true;
}

bool PriceLevel::remove(Order& order) noexcept {
    // Order must actually belong to this level.
    if (order.level_ != this) {
        return false;
    }

    // Connect previous order to next order.
    if (order.prev_ != nullptr) {
        order.prev_->next_ = order.next_;
    } else {
        // Removing the head.
        head_ = order.next_;
    }

    // Connect next order to previous order.
    if (order.next_ != nullptr) {
        order.next_->prev_ = order.prev_;
    } else {
        // Removing the tail.
        tail_ = order.prev_;
    }

    // Clear the removed order's links.
    order.prev_ = nullptr;
    order.next_ = nullptr;
    order.level_ = nullptr;

    --order_count_;

    return true;
}

} // namespace simulator