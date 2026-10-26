#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

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

// How a book keeps its levels, chosen once per replay (spec §2.2-§2.4). List: each side a list, best first, its levels
// found through the level index. Ladder: the list and index, and each side a window of ladder_window ticks around its
// best, where a level is found and placed by slot.
enum class BookBackend : std::uint8_t {
    List,
    Ladder
};

// ponytail: one window width for every symbol; size it per symbol only if the churn benchmark shows recentring
// dominates.
inline constexpr std::size_t ladder_window = 256;

// How many entries each structure holds.
struct BookCapacity
{
    std::size_t orders{0};
    std::size_t order_index{0};
    std::size_t levels{0};
    std::size_t level_index{0};
};

// Everything the books of one replay share, reserved before it starts (spec §3.1). From --capacity C: C orders and an
// order index of C, and C levels and a level index of C, as every live level holds a live order; and for the ladder,
// the windows of `books` books, SymbolIds 0 to books - 1. Nothing here allocates after the constructor.
struct BookStorage
{
private:
    std::unique_ptr<LevelRef[]> windows_;  // ladder: 2 x ladder_window per book, untouched until its book clears them
    std::size_t books_;

public:
    OrderPool orders;
    OrderIndex order_index;
    LevelPool levels;
    LevelIndex level_index;
    BookBackend backend;

    explicit BookStorage(std::size_t capacity, BookBackend backend = BookBackend::List, std::size_t books = 1)
        : BookStorage(
              BookCapacity{.orders = capacity, .order_index = capacity, .levels = capacity, .level_index = capacity},
              backend, books)
    {}

    explicit BookStorage(const BookCapacity& capacity, BookBackend backend = BookBackend::List, std::size_t books = 1)
        : windows_(backend == BookBackend::Ladder
                       ? std::make_unique_for_overwrite<LevelRef[]>(2 * ladder_window * books)
                       : nullptr),
          books_(books),
          orders(capacity.orders),
          order_index(capacity.order_index),
          levels(capacity.levels),
          level_index(capacity.level_index),
          backend(backend)
    {}

    BookStorage(const BookStorage&) = delete;
    BookStorage& operator=(const BookStorage&) = delete;

    // Ladder only: the ladder_window slots of `book`'s window on `side`. A book asks once, when it is made, so this is
    // checked in every build type.
    LevelRef* window(SymbolId book, Side side) noexcept
    {
        if (!windows_ || book >= books_) {
            std::fprintf(stderr, "book %zu has no ladder window: the storage holds %zu\n", std::size_t{book},
                         windows_ ? books_ : 0);
            std::abort();
        }
        return &windows_[(2 * std::size_t{book} + static_cast<std::size_t>(side)) * ladder_window];
    }

    // Every byte reserved for them.
    std::size_t bytes() const noexcept
    {
        return orders.bytes() + order_index.bytes() + levels.bytes() + level_index.bytes() + window_bytes(books_);
    }

    // The windows of `books` books: what the ladder reserves for books_, and touches for the books made.
    std::size_t window_bytes(std::size_t books) const noexcept
    {
        return windows_ ? 2 * ladder_window * books * sizeof(LevelRef) : 0;
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

}  // namespace simulator
