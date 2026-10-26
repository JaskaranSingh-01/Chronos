#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/orders/hash_index.hpp"
#include "simulator/orders/order.hpp"
#include "simulator/types/order_id.hpp"
#include "simulator/types/symbol_id.hpp"

namespace simulator {

// (book, OrderId) -> Order*, one table for every book. MarketEvent promises a live order's id unique only within its
// symbol, so the book is part of the key; the order holds it.
class OrderIndex
{
private:
    struct Slot
    {
        OrderId id{0};
        Order* order{nullptr};  // nullptr: a free slot

        bool empty() const noexcept
        {
            return order == nullptr;
        }

        std::uint64_t hash() const noexcept
        {
            return id;
        }
    };

    HashIndex<Slot> table_;

    // Reads the order's book only for a slot of the same id: the order a lookup reads next anyway.
    static auto matches(SymbolId book, OrderId id) noexcept
    {
        return [=](const Slot& slot) { return slot.id == id && slot.order->book() == book; };
    }

public:
    explicit OrderIndex(std::size_t capacity)
        : table_(capacity)
    {}

    // nullptr if absent.
    Order* find(SymbolId book, OrderId id) const noexcept
    {
        const Slot* slot = table_.find(id, matches(book, id));
        return slot != nullptr ? slot->order : nullptr;
    }

    // False, changing nothing, if full; the order's (book, id) is absent.
    bool add(Order& order) noexcept
    {
        return table_.add(Slot{.id = order.id(), .order = &order});
    }

    // False if absent.
    bool erase(SymbolId book, OrderId id) noexcept
    {
        return table_.erase(id, matches(book, id));
    }

    std::size_t size() const noexcept
    {
        return table_.size();
    }

    std::size_t capacity() const noexcept
    {
        return table_.capacity();
    }

    bool full() const noexcept
    {
        return table_.full();
    }
};

} // namespace simulator
