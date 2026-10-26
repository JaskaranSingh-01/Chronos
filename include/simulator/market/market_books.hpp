#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/orders/book_storage.hpp"

namespace simulator {

// One symbol's share of a multi-book run.
struct SymbolReport
{
    std::uint64_t events{0};
    std::array<std::uint64_t, apply_result_count> by_result{};  // index = ApplyResult
    std::size_t peak_orders{0};  // MarketBook::peak_order_count()
    std::size_t orders_left{0};  // MarketBook::order_count()

    std::uint64_t applied() const noexcept
    {
        return by_result[static_cast<std::size_t>(ApplyResult::Applied)];
    }

    std::uint64_t rejected() const noexcept
    {
        return events - applied();
    }

    bool operator==(const SymbolReport&) const = default;
};

// One MarketBook per registered instrument, indexed by SymbolId, sharing one BookStorage, each with its SymbolReport.
// A slot for every instrument the registry can hold is reserved up front, so a book made during a replay allocates
// nothing. Books are made lazily by find(); the caller keeps `registry` and `storage` alive.
class MarketBooks
{
private:
    struct Slot
    {
        std::optional<MarketBook> book;
        SymbolReport report;  // events and results; the order counts come from the book
    };

    const InstrumentRegistry& registry_;
    BookStorage& storage_;
    std::vector<Slot> slots_;  // index = SymbolId, one per instrument the registry can hold
    std::size_t size_{0};      // books made

public:
    MarketBooks(const InstrumentRegistry& registry, BookStorage& storage)
        : registry_(registry), storage_(storage), slots_(registry.capacity())
    {}

    // Makes the books of instruments registered since the last call, up to `count` books in all.
    void sync(std::size_t count = SIZE_MAX) noexcept
    {
        for (const std::size_t end = std::min(count, registry_.size()); size_ < end; ++size_) {
            slots_[size_].book.emplace(static_cast<SymbolId>(size_), storage_);
        }
    }

    // nullptr if `id` is not registered. Makes the books up to `id` only: a decoder may have registered more.
    MarketBook* find(SymbolId id) noexcept
    {
        if (id >= size_) {
            sync(std::size_t{id} + 1);
        }
        return id < size_ ? &*slots_[id].book : nullptr;
    }

    // Counts an event routed to book `id` and its result; an event of no book counts nowhere.
    void count(SymbolId id, ApplyResult result) noexcept
    {
        if (id < size_) {
            ++slots_[id].report.events;
            ++slots_[id].report.by_result[static_cast<std::size_t>(result)];
        }
    }

    SymbolReport report(SymbolId id) const noexcept
    {
        SymbolReport report = slots_[id].report;
        report.peak_orders = slots_[id].book->peak_order_count();
        report.orders_left = slots_[id].book->order_count();
        return report;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }
    const MarketBook& operator[](SymbolId id) const noexcept
    {
        return *slots_[id].book;
    }
    const OrderPool& pool() const noexcept
    {
        return storage_.orders;
    }

    // What the slots reserve.
    std::size_t bytes() const noexcept
    {
        return slots_.capacity() * sizeof(Slot);
    }

    // Checks every book, then the shared storage against all of them. On failure `symbol`
    // is the failing book, or size() for PoolCountMismatch and LevelCountMismatch.
    bool check_invariants(InvariantError& error, SymbolId& symbol, bool rth = false) const noexcept
    {
        std::size_t orders = 0;
        std::size_t levels = 0;

        for (SymbolId id = 0; id < size_; ++id) {
            const MarketBook& book = *slots_[id].book;
            if (!book.check_invariants(error, rth)) {
                symbol = id;
                return false;
            }
            orders += book.order_count();
            levels += book.level_count();
        }

        error = InvariantError::None;
        if (storage_.orders.in_use() != orders || storage_.order_index.size() != orders) {
            error = InvariantError::PoolCountMismatch;
        } else if (storage_.levels.in_use() != levels || storage_.level_index.size() != levels) {
            error = InvariantError::LevelCountMismatch;
        }
        if (error != InvariantError::None) {
            symbol = static_cast<SymbolId>(size_);
        }
        return error == InvariantError::None;
    }

    // FNV-1a over (id, last_sequence, digest) of every book in SymbolId order.
    // last_sequence() keeps a book that ends the day empty from hashing like an unused one.
    std::uint64_t digest() const noexcept
    {
        std::uint64_t hash = fnv1a_offset_basis;

        for (SymbolId id = 0; id < size_; ++id) {
            fnv1a_mix(hash, id);
            fnv1a_mix(hash, slots_[id].book->last_sequence());
            fnv1a_mix(hash, slots_[id].book->digest());
        }
        return hash;
    }
};

} // namespace simulator
