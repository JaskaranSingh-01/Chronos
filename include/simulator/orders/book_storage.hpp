#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/orders/level_index.hpp"
#include "simulator/orders/level_pool.hpp"
#include "simulator/orders/order_index.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {

// The structures the books of a replay share; a full one stops the replay (spec §3.2).
enum class BookStructure : std::uint8_t {
    None,
    OrderPool,
    OrderIndex,
    LevelPool,
    LevelIndex
};

// How many entries each structure holds.
struct BookCapacity
{
    std::size_t orders{0};
    std::size_t order_index{0};
    std::size_t levels{0};
    std::size_t level_index{0};
};

// Everything the books of one replay share, reserved before it starts (spec §3.1). From --capacity C: C orders and an
// order index of C, and C levels and a level index of C, as every live level holds a live order. Nothing here
// allocates after the constructor.
struct BookStorage
{
    OrderPool orders;
    OrderIndex order_index;
    LevelPool levels;
    LevelIndex level_index;

    explicit BookStorage(std::size_t capacity)
        : BookStorage(
              BookCapacity{.orders = capacity, .order_index = capacity, .levels = capacity, .level_index = capacity})
    {}

    explicit BookStorage(const BookCapacity& capacity)
        : orders(capacity.orders),
          order_index(capacity.order_index),
          levels(capacity.levels),
          level_index(capacity.level_index)
    {}

    BookStorage(const BookStorage&) = delete;
    BookStorage& operator=(const BookStorage&) = delete;

    // Every byte reserved for them.
    std::size_t bytes() const noexcept
    {
        return orders.bytes() + order_index.bytes() + levels.bytes() + level_index.bytes();
    }

    // The entries `structure` holds; 0 for None.
    std::size_t capacity(BookStructure structure) const noexcept
    {
        switch (structure) {
        case BookStructure::None:
            break;
        case BookStructure::OrderPool:
            return orders.capacity();
        case BookStructure::OrderIndex:
            return order_index.capacity();
        case BookStructure::LevelPool:
            return levels.capacity();
        case BookStructure::LevelIndex:
            return level_index.capacity();
        }
        return 0;
    }
};

} // namespace simulator
