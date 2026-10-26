#include "simulator/market/market_book.hpp"

#include <algorithm>
#include <cassert>

namespace simulator {

MarketBook::MarketBook(SymbolId symbol, std::size_t capacity)
    : symbol_(symbol), owned_(std::make_unique<BookStorage>(capacity)), storage_(owned_.get()), book_(symbol, *storage_)
{}

MarketBook::MarketBook(SymbolId symbol, BookStorage& storage) noexcept
    : symbol_(symbol), storage_(&storage), book_(symbol, storage)
{}

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
    case MarketEventType::Clear:
        return clear();
    case MarketEventType::Modify:
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

ApplyResult MarketBook::full(BookStructure structure) noexcept
{
    exhausted_ = structure;
    return ApplyResult::CapacityExhausted;
}

ApplyResult MarketBook::add(const MarketEvent& event) noexcept
{
    if (event.quantity.is_zero()) {
        return ApplyResult::ZeroQuantity;
    }

    if (book_.find(event.order_id) != nullptr) {
        return ApplyResult::DuplicateOrder;
    }

    if (storage_->orders.available() == 0) {
        return full(BookStructure::OrderPool);
    }

    if (const BookStructure structure = book_.room(event.side, event.price); structure != BookStructure::None) {
        return full(structure);
    }

    Order* order = storage_->orders.acquire(event.order_id, event.side, event.price, event.quantity, event.timestamp);
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
        [[maybe_unused]] const bool released = storage_->orders.release(*order);
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
    [[maybe_unused]] const bool released = storage_->orders.release(*order);
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

    if (const BookStructure structure = book_.room(side, event.price, old_order); structure != BookStructure::None) {
        return full(structure);
    }

    [[maybe_unused]] const bool erased = book_.erase(event.order_id);
    [[maybe_unused]] const bool released = storage_->orders.release(*old_order);

    // Cannot fail: the old order's slot was just freed.
    Order* order = storage_->orders.acquire(event.new_order_id, side, event.price, event.quantity, event.timestamp);
    assert(erased && released && order != nullptr);

    insert(*order);  // fresh sequence: back of the queue
    return ApplyResult::Applied;
}

// A new day: every order leaves as a Delete would; the book trades and sequences from 1, as a fresh one does.
ApplyResult MarketBook::clear() noexcept
{
    while (const Order* order = book_.best_bid() != nullptr ? book_.best_bid() : book_.best_ask()) {
        [[maybe_unused]] const ApplyResult erased = erase(MarketEvent{.order_id = order->id()});
        assert(erased == ApplyResult::Applied);
    }
    trading_state_ = TradingState::Trading;
    next_sequence_ = 1;
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
    const PriceLevel* level = book_.level(side, price);
    return level != nullptr ? level->total_quantity() : 0;
}

bool MarketBook::is_crossed() const noexcept
{
    const PriceLevel* bid = book_.best(Side::Buy);
    const PriceLevel* ask = book_.best(Side::Sell);
    return bid != nullptr && ask != nullptr && bid->price() > ask->price();
}

bool MarketBook::is_locked() const noexcept
{
    const PriceLevel* bid = book_.best(Side::Buy);
    const PriceLevel* ask = book_.best(Side::Sell);
    return bid != nullptr && ask != nullptr && bid->price() == ask->price();
}

bool MarketBook::check_invariants(InvariantError& error, bool rth) const noexcept
{
    std::size_t linked = 0;
    std::size_t levels = 0;

    auto check_side = [&](Side side) {
        const PriceLevel* before = nullptr;
        for (const PriceLevel* level = book_.best(side); level != nullptr; before = level, level = level->worse()) {
            const Price price = level->price();
            if (level->better() != before ||
                (before != nullptr && (side == Side::Buy ? before->price() <= price : before->price() >= price))) {
                return InvariantError::LevelOutOfOrder;
            }

            if (book_.level(side, price) != level) {
                return InvariantError::LevelNotIndexed;
            }

            if (level->empty()) {
                return InvariantError::EmptyLevel;
            }

            std::uint64_t total = 0;
            std::uint64_t previous_sequence = 0;  // sequences start at 1

            for (const Order* order = level->front(); order != nullptr; order = order->next()) {
                if (book_.find(order->id()) != order) {
                    return InvariantError::LevelWithoutIndex;
                }

                if (order->level() != level || order->price() != price || order->side() != side) {
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

            if (total != level->total_quantity()) {
                return InvariantError::LevelTotalMismatch;
            }

            linked += level->order_count();
            ++levels;
        }

        return InvariantError::None;
    };

    error = check_side(Side::Buy);

    if (error == InvariantError::None) {
        error = check_side(Side::Sell);
    }

    if (error == InvariantError::None && linked != book_.size()) {
        error = InvariantError::IndexWithoutLevel;
    }

    if (error == InvariantError::None && owned_ &&
        (storage_->orders.in_use() != book_.size() || storage_->order_index.size() != book_.size())) {
        error = InvariantError::PoolCountMismatch;
    }

    if (error == InvariantError::None && owned_ &&
        (storage_->levels.in_use() != levels || storage_->level_index.size() != levels)) {
        error = InvariantError::LevelCountMismatch;
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

    for (const Side side : {Side::Buy, Side::Sell}) {
        for (const PriceLevel* level = book_.best(side); level != nullptr; level = level->worse()) {
            for (const Order* order = level->front(); order != nullptr; order = order->next()) {
                mix(static_cast<std::uint64_t>(order->side()));
                mix(static_cast<std::uint64_t>(level->price().ticks));
                mix(order->id());
                mix(order->remaining_quantity().value());
                mix(order->sequence());
            }
        }
    }

    return hash;
}

} // namespace simulator
