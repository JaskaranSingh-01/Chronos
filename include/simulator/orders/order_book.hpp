#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "simulator/orders/book_storage.hpp"
#include "simulator/orders/order.hpp"
#include "simulator/orders/price_level.hpp"
#include "simulator/types/order_id.hpp"
#include "simulator/types/symbol_id.hpp"

namespace simulator {

// Where an order book's backend comes from: its storage, at run time (Selected, the engine's), or the compile-time
// choice of List or Ladder, which only the benchmark that prices the run-time choice uses (spec §2.4).
enum class Dispatch : std::uint8_t {
    Selected,
    List,
    Ladder
};

// One symbol's price levels over Orders it links but does not own. Each side is a list of levels, best first,
// kept in the storage's level pool; the storage's indices, which every book shares, find an order or a level
// (spec §2.2). With the ladder backend, each side also keeps a window of ladder_window slots a step apart,
// recentred on its best whenever the best leaves the window's middle half, where a level is found by its slot
// and placed by the nearest slot set towards the best (spec §2.3).
template <Dispatch D>
class BasicOrderBook
{
private:
    SymbolId symbol_;
    BookStorage& storage_;
    std::array<PriceLevel*, 2> best_{};    // index = Side: the head of its list
    std::array<LevelRef*, 2> window_{};    // ladder: each side's slots in the storage, a level's ref or 0
    std::array<std::uint64_t, 2> low_{};   // ladder: the price of each window's slot 0, as unsigned ticks
    std::uint64_t step_;                   // ladder: the ticks between slots
    std::size_t size_{0};
    bool ladder_;                          // the storage's backend, which Selected reads

    bool ladder() const noexcept
    {
        if constexpr (D == Dispatch::Selected) {
            return ladder_;
        } else {
            return D == Dispatch::Ladder;
        }
    }

    std::size_t slot(Side side, Price price) const noexcept;  // ladder_window if outside the window or off grid
    bool central(Side side, Price price) const noexcept;      // in the window's middle half, on grid or not
    void recentre(Side side) noexcept;
    PriceLevel* find_level(Side side, Price price) const noexcept;
    PriceLevel& new_level(Side side, Price price) noexcept;
    void drop_level(Side side, PriceLevel& level) noexcept;
    bool remove(Order& order) noexcept;  // unlinks it and drops it from the index

public:
    // With the ladder backend, `symbol` is below the storage's book count, and the book clears its own windows;
    // its slots are `step` ticks apart (ladder_step()).
    BasicOrderBook(SymbolId symbol, BookStorage& storage, std::uint64_t step = 1) noexcept;

    BasicOrderBook(const BasicOrderBook&) = delete;
    BasicOrderBook& operator=(const BasicOrderBook&) = delete;

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

    // True for the list. For the ladder, true if every level inside its side's window is in its slot, every slot
    // names one, and each side's best is in its window's middle half (spec §2.5). O(levels).
    bool windows_hold() const noexcept;

    // False if `id` is unknown or the order rejects the change.
    // An order reduced to zero leaves the book.
    bool execute(OrderId id, Quantity quantity) noexcept;
    bool cancel(OrderId id, Quantity quantity) noexcept;
    bool erase(OrderId id) noexcept;
};

extern template class BasicOrderBook<Dispatch::Selected>;
extern template class BasicOrderBook<Dispatch::List>;
extern template class BasicOrderBook<Dispatch::Ladder>;

// The engine's: its backend comes from its storage.
using OrderBook = BasicOrderBook<Dispatch::Selected>;

}  // namespace simulator
