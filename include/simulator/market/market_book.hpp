#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "simulator/market/market_event.hpp"
#include "simulator/orders/order_book.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {

// Why apply() accepted or rejected an event. Every rejection leaves the book
// unchanged; the caller counts results by reason.
enum class ApplyResult : std::uint8_t {
    Applied,           // also Trade, which never touches the book
    WrongSymbol,       // event.symbol is not this book's symbol
    UnknownOrder,      // Execute/Cancel/Delete/Replace of an id not in the book
    DuplicateOrder,    // Add (or Replace's new id) already in the book
    ZeroQuantity,      // Add/Execute/Cancel/Replace with quantity 0
    ExceedsRemaining,  // Execute/Cancel of more than the order's remaining
    InvalidReplace,    // Replace with new_order_id == order_id
    PoolExhausted,     // Add with no free order slot
    Unsupported        // Modify, Clear: until the first source that emits them lands
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
    PoolCountMismatch,      // pool slots in use != order_count()
    CrossedInRth            // best bid > best ask during regular trading hours
};

// Regular trading hours, [09:30, 16:00) exchange-local; session_midnight from
// session_midnight_ns(). Pre-market books may legitimately lock or cross.
constexpr bool in_rth(Timestamp now, Timestamp session_midnight) noexcept
{
    const Timestamp since_midnight = now - session_midnight;
    return since_midnight >= RTH_OPEN_NS && since_midnight < RTH_CLOSE_NS;
}

// The historical L3 book of one symbol. apply() is the only mutator; every
// other component reads it through a const MarketBook&.
class MarketBook
{
private:
    SymbolId symbol_;
    OrderPool pool_;
    OrderBook book_;
    std::uint64_t next_sequence_{1};
    std::size_t peak_order_count_{0};

    ApplyResult add(const MarketEvent& event) noexcept;
    ApplyResult reduce(const MarketEvent& event) noexcept;  // Execute, Cancel
    ApplyResult erase(const MarketEvent& event) noexcept;   // Delete
    ApplyResult replace(const MarketEvent& event) noexcept;
    void insert(Order& order) noexcept;                     // activate, sequence, link

public:
    // Reserves `order_capacity` order slots up front.
    MarketBook(SymbolId symbol, std::size_t order_capacity);

    ApplyResult apply(const MarketEvent& event) noexcept;

    const Order* find(OrderId id) const noexcept { return book_.find(id); }

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
        auto visit = [&f](const auto& levels) {
            for (const auto& [price, level] : levels) {
                if (!f(LevelInfo{price, level.total_quantity(), level.order_count()})) {
                    return;
                }
            }
        };
        side == Side::Buy ? visit(book_.bids()) : visit(book_.asks());
    }

    // Sequence of the most recent Add or Replace; 0 before the first.
    std::uint64_t last_sequence() const noexcept { return next_sequence_ - 1; }
    std::size_t order_count() const noexcept { return book_.size(); }
    std::size_t peak_order_count() const noexcept { return peak_order_count_; }

    bool is_crossed() const noexcept;  // best bid >  best ask
    bool is_locked() const noexcept;   // best bid == best ask

    // O(orders). On failure writes the first broken invariant to `error`.
    // A crossed book fails only when `rth` is true (see in_rth).
    bool check_invariants(InvariantError& error, bool rth = false) const noexcept;

    // FNV-1a over (side, price, id, remaining, sequence) in book order: bids
    // best first, then asks. Equal for equal books; compares runs.
    std::uint64_t digest() const noexcept;
};

} // namespace simulator
