#include "simulator/market/market_book.hpp"

#include <algorithm>
#include <cassert>

namespace simulator {

MarketBook::MarketBook(SymbolId symbol, std::size_t order_capacity)
    : symbol_(symbol), owned_pool_(std::in_place, order_capacity), pool_(&*owned_pool_), book_(order_capacity) {}

MarketBook::MarketBook(SymbolId symbol, OrderPool& pool) noexcept
    : symbol_(symbol), pool_(&pool) {}

// Every case validates before mutating, so a rejected event leaves the book unchanged.
ApplyResult MarketBook::apply(const MarketEvent& event) noexcept
{
    if (event.symbol != symbol_) {
        return ApplyResult::WrongSymbol;
    }

    switch (event.type) {
    case MarketEventType::Add:
        return add(event);
    case MarketEventType::Execute:
    case MarketEventType::Cancel:
        return reduce(event);
    case MarketEventType::Delete:
        return erase(event);
    case MarketEventType::Replace:
        return replace(event);
    case MarketEventType::Trade:
        return ApplyResult::Applied;  // not tied to a visible order
    case MarketEventType::Status:
        trading_state_ = event.trading_state;
        return ApplyResult::Applied;
    case MarketEventType::Modify:
    case MarketEventType::Clear:
        break;
    }

    return ApplyResult::Unsupported;
}

void MarketBook::insert(Order& order) noexcept
{
    [[maybe_unused]] const bool activated = order.activate();
    order.set_sequence(next_sequence_++);
    // Callers validated quantity and id, so the book cannot refuse it.
    [[maybe_unused]] const bool added = book_.add(order);
    assert(activated && added);

    peak_order_count_ = std::max(peak_order_count_, book_.size());
}

ApplyResult MarketBook::add(const MarketEvent& event) noexcept
{
    if (event.quantity.is_zero()) {
        return ApplyResult::ZeroQuantity;
    }

    if (book_.find(event.order_id) != nullptr) {
        return ApplyResult::DuplicateOrder;
    }

    Order* order = pool_->acquire(event.order_id, event.side, event.price, event.quantity, event.timestamp);

    if (order == nullptr) {
        return ApplyResult::PoolExhausted;
    }

    insert(*order);
    return ApplyResult::Applied;
}

ApplyResult MarketBook::reduce(const MarketEvent& event) noexcept
{
    Order* order = book_.find(event.order_id);

    if (order == nullptr) {
        return ApplyResult::UnknownOrder;
    }

    if (event.quantity.is_zero()) {
        return ApplyResult::ZeroQuantity;
    }

    if (event.quantity > order->remaining_quantity()) {
        return ApplyResult::ExceedsRemaining;
    }

    // Price is ignored: the resting order's price is authoritative.
    [[maybe_unused]] const bool reduced = event.type == MarketEventType::Execute
        ? book_.execute(event.order_id, event.quantity)
        : book_.cancel(event.order_id, event.quantity);
    assert(reduced);

    // At zero the book has already unlinked it.
    if (order->remaining_quantity().is_zero()) {
        [[maybe_unused]] const bool released = pool_->release(*order);
        assert(released);
    }

    return ApplyResult::Applied;
}

ApplyResult MarketBook::erase(const MarketEvent& event) noexcept
{
    Order* order = book_.find(event.order_id);

    if (order == nullptr) {
        return ApplyResult::UnknownOrder;
    }

    // Unlink before delete_order(), which zeroes the remaining the level subtracts on unlink.
    [[maybe_unused]] const bool erased = book_.erase(event.order_id);
    [[maybe_unused]] const bool deleted = order->delete_order();
    [[maybe_unused]] const bool released = pool_->release(*order);
    assert(erased && deleted && released);

    return ApplyResult::Applied;
}

ApplyResult MarketBook::replace(const MarketEvent& event) noexcept
{
    Order* old_order = book_.find(event.order_id);

    if (old_order == nullptr) {
        return ApplyResult::UnknownOrder;
    }

    if (event.new_order_id == event.order_id) {
        return ApplyResult::InvalidReplace;
    }

    if (book_.find(event.new_order_id) != nullptr) {
        return ApplyResult::DuplicateOrder;
    }

    if (event.quantity.is_zero()) {
        return ApplyResult::ZeroQuantity;
    }

    const Side side = old_order->side();

    [[maybe_unused]] const bool erased = book_.erase(event.order_id);
    [[maybe_unused]] const bool released = pool_->release(*old_order);

    // Cannot fail: the old order's slot was just freed.
    Order* order = pool_->acquire(event.new_order_id, side, event.price, event.quantity, event.timestamp);
    assert(erased && released && order != nullptr);

    insert(*order);  // fresh sequence: back of the queue
    return ApplyResult::Applied;
}

bool MarketBook::best(Side side, LevelInfo& level) const noexcept
{
    return depth(side, std::span<LevelInfo>{&level, 1}) == 1;
}

std::size_t MarketBook::depth(Side side, std::span<LevelInfo> out) const noexcept
{
    std::size_t count = 0;

    visit_levels(side, [&](const LevelInfo& level) {
        if (count == out.size()) {
            return false;
        }
        out[count++] = level;
        return true;
    });

    return count;
}

std::uint64_t MarketBook::quantity_at(Side side, Price price) const noexcept
{
    auto at = [price](const auto& levels) -> std::uint64_t {
        const auto it = levels.find(price);
        return it == levels.end() ? 0 : it->second.total_quantity();
    };

    return side == Side::Buy ? at(book_.bids()) : at(book_.asks());
}

bool MarketBook::is_crossed() const noexcept
{
    return !book_.bids().empty() && !book_.asks().empty() &&
           book_.bids().begin()->first > book_.asks().begin()->first;
}

bool MarketBook::is_locked() const noexcept
{
    return !book_.bids().empty() && !book_.asks().empty() &&
           book_.bids().begin()->first == book_.asks().begin()->first;
}

bool MarketBook::check_invariants(InvariantError& error, bool rth) const noexcept
{
    std::size_t linked = 0;

    auto check_side = [&](const auto& levels, Side side) {
        for (const auto& [price, level] : levels) {
            if (level.empty()) {
                return InvariantError::EmptyLevel;
            }

            std::uint64_t total = 0;
            std::uint64_t previous_sequence = 0;  // sequences start at 1

            for (const Order* order = level.front(); order != nullptr; order = order->next()) {
                if (book_.find(order->id()) != order) {
                    return InvariantError::LevelWithoutIndex;
                }

                if (order->level() != &level || order->price() != price || order->side() != side) {
                    return InvariantError::IndexWithoutLevel;
                }

                const OrderState state = order->state();
                if ((state != OrderState::Active && state != OrderState::PartiallyFilled) ||
                    order->remaining_quantity().is_zero()) {
                    return InvariantError::InactiveOrderInBook;
                }

                if (order->sequence() <= previous_sequence) {
                    return InvariantError::SequenceNotIncreasing;
                }
                previous_sequence = order->sequence();

                total += order->remaining_quantity().value();
            }

            if (total != level.total_quantity()) {
                return InvariantError::LevelTotalMismatch;
            }

            linked += level.order_count();
        }

        return InvariantError::None;
    };

    error = check_side(book_.bids(), Side::Buy);

    if (error == InvariantError::None) {
        error = check_side(book_.asks(), Side::Sell);
    }

    if (error == InvariantError::None && linked != book_.size()) {
        error = InvariantError::IndexWithoutLevel;
    }

    if (error == InvariantError::None && owned_pool_ && pool_->in_use() != book_.size()) {
        error = InvariantError::PoolCountMismatch;
    }

    if (error == InvariantError::None && rth && trading_state_ == TradingState::Trading && is_crossed()) {
        error = InvariantError::CrossedInRth;
    }

    return error == InvariantError::None;
}

std::uint64_t MarketBook::digest() const noexcept
{
    std::uint64_t hash = fnv1a_offset_basis;
    auto mix = [&hash](std::uint64_t value) { fnv1a_mix(hash, value); };

    auto mix_side = [&mix](const auto& levels) {
        for (const auto& [price, level] : levels) {
            for (const Order* order = level.front(); order != nullptr; order = order->next()) {
                mix(static_cast<std::uint64_t>(order->side()));
                mix(static_cast<std::uint64_t>(price.ticks));
                mix(order->id());
                mix(order->remaining_quantity().value());
                mix(order->sequence());
            }
        }
    };

    mix_side(book_.bids());
    mix_side(book_.asks());

    return hash;
}

} // namespace simulator
