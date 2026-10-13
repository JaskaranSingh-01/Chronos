#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {

// One MarketBook per registered instrument, indexed by SymbolId, sharing one OrderPool.
// Books are created lazily by find(); the caller keeps `registry` and `pool` alive.
class MarketBooks
{
private:
    const InstrumentRegistry& registry_;
    OrderPool& pool_;
    std::deque<MarketBook> books_;  // index = SymbolId; a deque never moves its books

public:
    MarketBooks(const InstrumentRegistry& registry, OrderPool& pool) noexcept
        : registry_(registry), pool_(pool)
    {}

    // Adds the books of instruments registered since the last call.
    void sync()
    {
        while (books_.size() < registry_.size()) {
            books_.emplace_back(static_cast<SymbolId>(books_.size()), pool_);
        }
    }

    // nullptr if `id` is not registered.
    MarketBook* find(SymbolId id)
    {
        if (id >= books_.size()) {
            sync();
        }
        return id < books_.size() ? &books_[id] : nullptr;
    }

    std::size_t size() const noexcept
    {
        return books_.size();
    }
    const MarketBook& operator[](SymbolId id) const noexcept
    {
        return books_[id];
    }
    const OrderPool& pool() const noexcept
    {
        return pool_;
    }

    // Checks every book, then the shared pool against all of them. On failure `symbol`
    // is the failing book, or size() for PoolCountMismatch.
    bool check_invariants(InvariantError& error, SymbolId& symbol, bool rth = false) const noexcept
    {
        std::size_t orders = 0;

        for (SymbolId id = 0; id < books_.size(); ++id) {
            if (!books_[id].check_invariants(error, rth)) {
                symbol = id;
                return false;
            }
            orders += books_[id].order_count();
        }

        if (pool_.in_use() != orders) {
            error = InvariantError::PoolCountMismatch;
            symbol = static_cast<SymbolId>(books_.size());
            return false;
        }
        return true;
    }

    // FNV-1a over (id, last_sequence, digest) of every book in SymbolId order.
    // last_sequence() keeps a book that ends the day empty from hashing like an unused one.
    std::uint64_t digest() const noexcept
    {
        std::uint64_t hash = fnv1a_offset_basis;

        for (SymbolId id = 0; id < books_.size(); ++id) {
            fnv1a_mix(hash, id);
            fnv1a_mix(hash, books_[id].last_sequence());
            fnv1a_mix(hash, books_[id].digest());
        }
        return hash;
    }
};

} // namespace simulator
