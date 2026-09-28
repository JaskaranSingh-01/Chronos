#pragma once

#include <cstdint>

#include "simulator/orders/order.hpp"
#include "simulator/types/price.hpp"

namespace simulator {

class PriceLevel {
private:
    Price price_{};

    Order* head_{nullptr};
    Order* tail_{nullptr};

    std::uint32_t order_count_{0};

public:
    explicit PriceLevel(Price price) noexcept
        : price_(price) {}

    // Add an order to the back of the FIFO queue.
    bool add(Order& order) noexcept;

    // Remove an order from anywhere in the FIFO queue.
    bool remove(Order& order) noexcept;

    // First order in time priority.
    Order* front() noexcept {
        return head_;
    }

    const Order* front() const noexcept {
        return head_;
    }

    constexpr Price price() const noexcept {
        return price_;
    }

    constexpr std::uint32_t order_count() const noexcept {
        return order_count_;
    }

    constexpr bool empty() const noexcept {
        return head_ == nullptr;
    }
};

} // namespace simulator