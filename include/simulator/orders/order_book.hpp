#pragma once

#include <array>
#include <cstddef>

#include "simulator/orders/book_storage.hpp"
#include "simulator/orders/order.hpp"
#include "simulator/orders/price_level.hpp"
#include "simulator/types/order_id.hpp"
#include "simulator/types/symbol_id.hpp"

namespace simulator {

// One symbol's price levels over Orders it links but does not own. Each side is a list of levels, best first, kept in
// the storage's level pool; the storage's indices, which every book shares, find an order or a level (spec §2.2).
class OrderBook
{
private:
    SymbolId symbol_;
    BookStorage& storage_;
    std::array<PriceLevel*, 2> best_{};  // index = Side: the head of its list
    std::size_t size_{0};

    PriceLevel* find_level(Side side, Price price) const noexcept;
    PriceLevel& new_level(Side side, Price price) noexcept;
    void drop_level(Side side, PriceLevel& level) noexcept;
    bool remove(Order& order) noexcept;  // unlinks it and drops it from the index

public:
    OrderBook(SymbolId symbol, BookStorage& storage) noexcept
        : symbol_(symbol), storage_(storage)
    {}

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // The first structure too full for one more order at (side, price), or None. `leaving`, an order of this book
    // that a Replace removes first, frees its index entry and, if it is alone at its price, its level.
    BookStructure room(Side side, Price price, const Order* leaving = nullptr) const noexcept;

    // False, changing nothing, if the order is not Active or PartiallyFilled, has nothing remaining, is linked already,
    // shares its id with an order in the book, or room() names a full structure.
    bool add(Order& order) noexcept;
    bool cancel(OrderId id) noexcept;
    Order* find(OrderId id) noexcept;
    const Order* find(OrderId id) const noexcept;
    const Order* best_bid() const noexcept;
    const Order* best_ask() const noexcept;
    bool empty() const noexcept;
    std::size_t size() const noexcept
    {
        return size_;
    }

    // The best level of `side`, nullptr if the side is empty; PriceLevel::worse() walks the rest.
    const PriceLevel* best(Side side) const noexcept
    {
        return best_[static_cast<std::size_t>(side)];
    }

    // The level at `price`, nullptr if there is none.
    const PriceLevel* level(Side side, Price price) const noexcept
    {
        return find_level(side, price);
    }

    // Levels on both sides; O(levels).
    std::size_t level_count() const noexcept;

    // False if `id` is unknown or the order rejects the change.
    // An order reduced to zero leaves the book.
    bool execute(OrderId id, Quantity quantity) noexcept;
    bool cancel(OrderId id, Quantity quantity) noexcept;
    bool erase(OrderId id) noexcept;
};

}  // namespace simulator
