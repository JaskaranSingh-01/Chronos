#pragma once

#include <cstddef>
#include <memory>

#include "simulator/orders/order.hpp"

namespace simulator {

// Fixed-capacity Order storage with a free list; allocates only in the constructor.
class OrderPool
{
private:
    struct Slot
    {
        Order order;
        Slot* next{nullptr};
        bool is_available{true};
    };

    std::unique_ptr<Slot[]> storage_;

    Slot* free_list_{nullptr};

    std::size_t capacity_{0};
    std::size_t available_{0};

public:
    explicit OrderPool(std::size_t capacity);

    ~OrderPool() = default;

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    OrderPool(OrderPool&&) = delete;
    OrderPool& operator=(OrderPool&&) = delete;

    // nullptr when every slot is in use.
    Order* acquire(
        OrderId id,
        Side side,
        Price price,
        Quantity quantity,
        Timestamp timestamp
    ) noexcept;

    // False if `order` is not an in-use slot of this pool or is still linked into a level.
    bool release(Order& order) noexcept;

    constexpr std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    constexpr std::size_t available() const noexcept
    {
        return available_;
    }

    constexpr std::size_t in_use() const noexcept
    {
        return capacity_ - available_;
    }
};

} // namespace simulator
