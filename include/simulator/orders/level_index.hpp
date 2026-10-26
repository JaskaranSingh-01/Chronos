#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/orders/hash_index.hpp"
#include "simulator/orders/price_level.hpp"
#include "simulator/types/enums.hpp"
#include "simulator/types/price.hpp"
#include "simulator/types/symbol_id.hpp"

namespace simulator {

// (book, side, price) -> the level at that price, one table for every book (spec §2.2).
class LevelIndex
{
private:
    struct Slot
    {
        Price price{};
        SymbolId book{0};
        std::uint32_t entry{0};  // level << 1 | side; 0 is a free slot, as level 0 is none

        bool empty() const noexcept
        {
            return entry == 0;
        }

        std::uint64_t hash() const noexcept
        {
            return key(book, static_cast<Side>(entry & 1), price);
        }
    };

    HashIndex<Slot> table_;

    static std::uint64_t key(SymbolId book, Side side, Price price) noexcept
    {
        return (static_cast<std::uint64_t>(price.ticks) << 1 | static_cast<std::uint64_t>(side)) +
            std::uint64_t{book} * 0x9E37'79B9'7F4A'7C15ULL;
    }

    static auto matches(SymbolId book, Side side, Price price) noexcept
    {
        return [=](const Slot& slot) {
            return slot.price == price && slot.book == book && (slot.entry & 1) == static_cast<std::uint32_t>(side);
        };
    }

public:
    explicit LevelIndex(std::size_t capacity)
        : table_(capacity)
    {}

    // 0 if there is no level at (book, side, price).
    LevelRef find(SymbolId book, Side side, Price price) const noexcept
    {
        const Slot* slot = table_.find(key(book, side, price), matches(book, side, price));
        return slot != nullptr ? slot->entry >> 1 : 0;
    }

    // False, changing nothing, if full; (book, side, price) is absent and `level` is not 0.
    bool add(SymbolId book, Side side, Price price, LevelRef level) noexcept
    {
        return table_.add(Slot{.price = price, .book = book, .entry = level << 1 | static_cast<std::uint32_t>(side)});
    }

    // False if absent.
    bool erase(SymbolId book, Side side, Price price) noexcept
    {
        return table_.erase(key(book, side, price), matches(book, side, price));
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

    std::size_t bytes() const noexcept
    {
        return table_.bytes();
    }
};

} // namespace simulator
