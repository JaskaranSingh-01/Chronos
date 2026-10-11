#pragma once

#include <cassert>
#include <cstdint>

#include "simulator/orders/order.hpp"
#include "simulator/types/price.hpp"

namespace simulator {

class PriceLevel {
private:
    Price price_{};

    Order* head_{nullptr};
    Order* tail_{nullptr};

    std::uint64_t total_quantity_{0};  // sum of remaining over the level
    std::uint32_t order_count_{0};

public:
    explicit PriceLevel(Price price) noexcept
        : price_(price) {}

    bool add(Order& order) noexcept;

    bool remove(Order& order) noexcept;

    // A partial execute or cancel of an order in this level: call after the
    // order's remaining has dropped by `quantity`.
    void reduce(Quantity quantity) noexcept {
        assert(quantity.value() <= total_quantity_);
        total_quantity_ -= quantity.value();
    }

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

    constexpr std::uint64_t total_quantity() const noexcept {
        return total_quantity_;
    }

    constexpr bool empty() const noexcept {
        return head_ == nullptr;
    }
};

} // namespace simulator