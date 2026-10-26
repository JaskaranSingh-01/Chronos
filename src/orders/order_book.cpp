#include "simulator/orders/order_book.hpp"

#include <cassert>

namespace simulator {

namespace {

bool better(Side side, Price a, Price b) noexcept
{
    return side == Side::Buy ? a > b : a < b;
}

} // namespace

PriceLevel* OrderBook::find_level(Side side, Price price) const noexcept
{
    const LevelRef ref = storage_.level_index.find(symbol_, side, price);
    return ref != 0 ? &storage_.levels.at(ref) : nullptr;
}

// Links a new level after every level better than it, found by walking from the best: O(levels passed).
PriceLevel& OrderBook::new_level(Side side, Price price) noexcept
{
    PriceLevel& level = *storage_.levels.acquire(price);  // room() saw a free slot
    [[maybe_unused]] const bool indexed = storage_.level_index.add(symbol_, side, price, storage_.levels.ref(level));
    assert(indexed);

    PriceLevel*& best = best_[static_cast<std::size_t>(side)];
    PriceLevel* before = nullptr;
    PriceLevel* after = best;
    while (after != nullptr && !better(side, price, after->price())) {
        before = after;
        after = after->worse_;
    }
    level.better_ = before;
    level.worse_ = after;
    (before != nullptr ? before->worse_ : best) = &level;
    if (after != nullptr) {
        after->better_ = &level;
    }
    return level;
}

void OrderBook::drop_level(Side side, PriceLevel& level) noexcept
{
    (level.better_ != nullptr ? level.better_->worse_ : best_[static_cast<std::size_t>(side)]) = level.worse_;
    if (level.worse_ != nullptr) {
        level.worse_->better_ = level.better_;
    }
    level.better_ = nullptr;
    level.worse_ = nullptr;
    [[maybe_unused]] const bool erased = storage_.level_index.erase(symbol_, side, level.price());
    assert(erased);
    storage_.levels.release(level);
}

// Leaves the order's state to the caller.
bool OrderBook::remove(Order& order) noexcept
{
    PriceLevel* level = order.level_;

    if (level == nullptr || !level->remove(order)) {
        return false;
    }

    if (level->empty()) {
        drop_level(order.side(), *level);
    }

    [[maybe_unused]] const bool erased = storage_.order_index.erase(symbol_, order.id());
    assert(erased);
    --size_;
    return true;
}

BookStructure OrderBook::room(Side side, Price price, const Order* leaving) const noexcept
{
    if (leaving == nullptr && storage_.order_index.full()) {
        return BookStructure::OrderIndex;
    }
    if (!storage_.levels.full() && !storage_.level_index.full()) {
        return BookStructure::None;
    }
    if (find_level(side, price) != nullptr || (leaving != nullptr && leaving->level()->order_count() == 1)) {
        return BookStructure::None;
    }
    return storage_.levels.full() ? BookStructure::LevelPool : BookStructure::LevelIndex;
}

bool OrderBook::add(Order& order) noexcept
{
    if (order.remaining_quantity().is_zero()) {
        return false;
    }

    if (order.state() != OrderState::Active && order.state() != OrderState::PartiallyFilled) {
        return false;
    }

    if (order.level_ != nullptr || find(order.id()) != nullptr ||
        room(order.side(), order.price()) != BookStructure::None) {
        return false;
    }

    PriceLevel* level = find_level(order.side(), order.price());
    if (level == nullptr) {
        level = &new_level(order.side(), order.price());
    }
    [[maybe_unused]] const bool linked = level->add(order);
    order.set_book(symbol_);
    [[maybe_unused]] const bool indexed = storage_.order_index.add(order);
    assert(linked && indexed);
    ++size_;

    return true;
}

bool OrderBook::cancel(OrderId id) noexcept
{
    Order* order = find(id);

    if (order == nullptr) {
        return false;
    }

    if (order->state() != OrderState::Active && order->state() != OrderState::PartiallyFilled) {
        return false;
    }

    if (!remove(*order)) {
        return false;
    }

    return order->cancel();
}

Order* OrderBook::find(OrderId id) noexcept
{
    return storage_.order_index.find(symbol_, id);
}

const Order* OrderBook::find(OrderId id) const noexcept
{
    return storage_.order_index.find(symbol_, id);
}

const Order* OrderBook::best_bid() const noexcept
{
    const PriceLevel* level = best(Side::Buy);
    return level != nullptr ? level->front() : nullptr;
}

const Order* OrderBook::best_ask() const noexcept
{
    const PriceLevel* level = best(Side::Sell);
    return level != nullptr ? level->front() : nullptr;
}

bool OrderBook::empty() const noexcept
{
    return size_ == 0;
}

std::size_t OrderBook::level_count() const noexcept
{
    std::size_t count = 0;
    for (const PriceLevel* first : best_) {
        for (const PriceLevel* level = first; level != nullptr; level = level->worse_) {
            ++count;
        }
    }
    return count;
}

bool OrderBook::execute(OrderId id, Quantity quantity) noexcept
{
    Order* order = find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->execute(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Filled) {
        return true;
    }

    return remove(*order);
}

bool OrderBook::cancel(OrderId id, Quantity quantity) noexcept
{
    Order* order = find(id);

    if (order == nullptr) {
        return false;
    }

    if (!order->cancel(quantity)) {
        return false;
    }

    order->level_->reduce(quantity);

    if (order->state() != OrderState::Cancelled) {
        return true;
    }

    return remove(*order);
}

bool OrderBook::erase(OrderId id) noexcept
{
    Order* order = find(id);

    if (order == nullptr) {
        return false;
    }

    return remove(*order);
}

} // namespace simulator
