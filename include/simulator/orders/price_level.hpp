#pragma once

#include <cassert>
#include <cstdint>

#include "simulator/orders/order.hpp"
#include "simulator/types/price.hpp"

namespace simulator {

// A PriceLevel's slot in its LevelPool; 0 is none.
using LevelRef = std::uint32_t;

// FIFO of the orders at one price, intrusively linked through Order::prev_/next_, and a node of its side's list of
// levels, best first.
class PriceLevel
{
private:
    std::int64_t price_;  // Price::ticks: a plain integer, so a LevelPool slot needs no initialization

    Order* head_;
    Order* tail_;

    std::uint64_t total_quantity_;  // sum of remaining over the level
    std::uint32_t order_count_;

    PriceLevel* better_;  // the level before it in its side's list; nullptr at the best
    PriceLevel* worse_;   // the level after it; in a LevelPool's free list, the next free slot

    friend class OrderBook;
    friend class LevelPool;

public:
    // Uninitialized: a LevelPool slot, which the pool writes when it hands it out.
    PriceLevel() = default;

    explicit PriceLevel(Price price) noexcept
        : price_(price.ticks),
          head_(nullptr),
          tail_(nullptr),
          total_quantity_(0),
          order_count_(0),
          better_(nullptr),
          worse_(nullptr)
    {}

    bool add(Order& order) noexcept;

    bool remove(Order& order) noexcept;

    // Call after a partial execute or cancel drops an order's remaining by `quantity`.
    void reduce(Quantity quantity) noexcept
    {
        assert(quantity.value() <= total_quantity_);
        total_quantity_ -= quantity.value();
    }

    Order* front() noexcept
    {
        return head_;
    }

    const Order* front() const noexcept
    {
        return head_;
    }

    const PriceLevel* better() const noexcept
    {
        return better_;
    }

    const PriceLevel* worse() const noexcept
    {
        return worse_;
    }

    constexpr Price price() const noexcept
    {
        return Price{price_};
    }

    constexpr std::uint32_t order_count() const noexcept
    {
        return order_count_;
    }

    constexpr std::uint64_t total_quantity() const noexcept
    {
        return total_quantity_;
    }

    constexpr bool empty() const noexcept
    {
        return head_ == nullptr;
    }
};

} // namespace simulator
