#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "simulator/market/market_event.hpp"
#include "simulator/orders/book_storage.hpp"
#include "simulator/orders/order_book.hpp"

namespace simulator {

// Outcome of MarketBook::apply(). Every rejection leaves the book unchanged.
enum class ApplyResult : std::uint8_t {
    Applied,           // includes Trade and Status, which touch no order
    WrongSymbol,       // event.symbol is not this book's symbol
    UnknownOrder,      // Execute/Cancel/Delete/Replace of an id not in the book
    DuplicateOrder,    // Add (or Replace's new id) already in the book
    ZeroQuantity,      // Add/Execute/Cancel/Replace with quantity 0
    ExceedsRemaining,  // Execute/Cancel of more than the order's remaining
    InvalidReplace,    // Replace with new_order_id == order_id
    CapacityExhausted, // Add or Replace that a full pool or index has no room for; exhausted() names it
    Unsupported        // Modify
};

inline constexpr std::size_t apply_result_count = 9;

static_assert(static_cast<std::size_t>(ApplyResult::Unsupported) + 1 == apply_result_count);

struct LevelInfo
{
    Price price{};
    std::uint64_t quantity{0};     // sum of remaining shares
    std::uint32_t order_count{0};

    bool operator==(const LevelInfo&) const = default;
};

// The first broken invariant check_invariants() finds.
enum class InvariantError : std::uint8_t {
    None,
    IndexWithoutLevel,      // indexed orders not linked into levels, or linked under the wrong level
    LevelWithoutIndex,      // a linked order the index does not map to itself
    LevelTotalMismatch,     // level total != sum of remaining over the level
    EmptyLevel,
    SequenceNotIncreasing,  // FIFO broken: sequence must rise from head to tail
    InactiveOrderInBook,    // not Active/PartiallyFilled, or remaining == 0
    PoolCountMismatch,      // order pool slots in use or order index size != order_count() (of all books, if shared)
    CrossedInRth,           // best bid > best ask during regular trading hours, while Trading
    LevelOutOfOrder,        // a side's list not strictly best first, or its links disagree
    LevelNotIndexed,        // the level index does not map a level's price to it
    LevelCountMismatch,     // level pool slots in use or level index size != live levels (of all books, if shared)
    WindowMismatch          // ladder: a window slot and the level at its price disagree, or a best off-centre
};

// True in [09:30, 16:00) exchange-local; session_midnight comes from session_midnight_ns().
constexpr bool in_rth(Timestamp now, Timestamp session_midnight) noexcept
{
    const Timestamp since_midnight = now - session_midnight;
    return since_midnight >= RTH_OPEN_NS && since_midnight < RTH_CLOSE_NS;
}

// Initial value of an FNV-1a 64 hash.
inline constexpr std::uint64_t fnv1a_offset_basis = 14695981039346656037ULL;

// Mixes `value` into `hash` byte by byte, little-endian, so digests do not depend on the host.
constexpr void fnv1a_mix(std::uint64_t& hash, std::uint64_t value) noexcept
{
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xFFU;
        hash *= 1099511628211ULL;  // FNV-1a 64 prime
    }
}

// L3 book of one symbol, rebuilt from market events. apply() is the only mutator.
class MarketBook
{
private:
    SymbolId symbol_;
    TradingState trading_state_{TradingState::Trading};
    std::unique_ptr<BookStorage> owned_;  // null when the storage is shared
    BookStorage* storage_;                // owned_ or the shared one
    OrderBook book_;
    std::uint64_t next_sequence_{1};
    std::size_t peak_order_count_{0};
    BookStructure exhausted_{BookStructure::None};

    ApplyResult add(const MarketEvent& event) noexcept;
    ApplyResult reduce(const MarketEvent& event) noexcept;  // Execute, Cancel
    ApplyResult erase(const MarketEvent& event) noexcept;   // Delete
    ApplyResult replace(const MarketEvent& event) noexcept;
    ApplyResult clear() noexcept;
    ApplyResult full(BookStructure structure) noexcept;     // CapacityExhausted, naming `structure`
    void insert(Order& order) noexcept;                     // activate, sequence, link

public:
    // Reserves a BookStorage of its own for `capacity` orders up front.
    MarketBook(SymbolId symbol, std::size_t capacity, BookBackend backend = BookBackend::List);

    // Keeps its orders and levels in `storage`, which other books may share and which outlives the book.
    MarketBook(SymbolId symbol, BookStorage& storage) noexcept;

    MarketBook(const MarketBook&) = delete;
    MarketBook& operator=(const MarketBook&) = delete;

    ApplyResult apply(const MarketEvent& event) noexcept;

    SymbolId symbol() const noexcept
    {
        return symbol_;
    }

    // Trading until a Status event says otherwise.
    TradingState trading_state() const noexcept
    {
        return trading_state_;
    }
    const Order* find(OrderId id) const noexcept
    {
        return book_.find(id);
    }

    // False if the side is empty.
    bool best(Side side, LevelInfo& level) const noexcept;

    // Copies up to out.size() levels, best first; returns how many.
    std::size_t depth(Side side, std::span<LevelInfo> out) const noexcept;

    // Total remaining at `price`; 0 if there is no such level.
    std::uint64_t quantity_at(Side side, Price price) const noexcept;

    // Calls f(const LevelInfo&) best first; stops when f returns false.
    template <class F>
    void visit_levels(Side side, F&& f) const noexcept
    {
        for (const PriceLevel* level = book_.best(side); level != nullptr; level = level->worse()) {
            if (!f(LevelInfo{level->price(), level->total_quantity(), level->order_count()})) {
                return;
            }
        }
    }

    // Calls f(const Order&) for the orders at `price` on `side`, front of the queue first; stops when f returns false.
    template <class F>
    void visit_orders(Side side, Price price, F&& f) const noexcept
    {
        const PriceLevel* level = book_.level(side, price);
        for (const Order* order = level != nullptr ? level->front() : nullptr; order != nullptr;
             order = order->next()) {
            if (!f(*order)) {
                return;
            }
        }
    }

    // Sequence of the most recent Add or Replace; 0 before the first.
    std::uint64_t last_sequence() const noexcept
    {
        return next_sequence_ - 1;
    }
    std::size_t order_count() const noexcept
    {
        return book_.size();
    }
    std::size_t peak_order_count() const noexcept
    {
        return peak_order_count_;
    }
    std::size_t level_count() const noexcept
    {
        return book_.level_count();
    }

    // The structure the last CapacityExhausted found full; None before one.
    BookStructure exhausted() const noexcept
    {
        return exhausted_;
    }
    const BookStorage& storage() const noexcept
    {
        return *storage_;
    }

    bool is_crossed() const noexcept;  // best bid >  best ask
    bool is_locked() const noexcept;  // best bid == best ask

    // O(orders); on failure writes the first broken invariant to `error`. A crossed book fails only if `rth` and
    // Trading. The storage's counts are checked only for an owned storage (MarketBooks checks a shared one).
    bool check_invariants(InvariantError& error, bool rth = false) const noexcept;

    // FNV-1a over (side, price, id, remaining, sequence) of every order, bids then asks,
    // best first. Excludes the trading state.
    std::uint64_t digest() const noexcept;
};

}  // namespace simulator
