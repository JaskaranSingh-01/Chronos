#include "simulator/orders/order_book.hpp"

#include <algorithm>
#include <cassert>

namespace simulator {

namespace {

bool better(Side side, Price a, Price b) noexcept
{
    return side == Side::Buy ? a > b : a < b;
}

std::size_t index(Side side) noexcept
{
    return static_cast<std::size_t>(side);
}

} // namespace

template <Dispatch D>
BasicOrderBook<D>::BasicOrderBook(SymbolId symbol, BookStorage& storage, std::uint64_t step) noexcept
    : symbol_(symbol), storage_(storage), step_(step), ladder_(storage.backend == BookBackend::Ladder)
{
    assert(step_ != 0);
    assert(D == Dispatch::Selected || ladder() == ladder_);
    if (ladder()) {
        for (const Side side : {Side::Buy, Side::Sell}) {
            window_[index(side)] = storage.window(symbol, side);
            std::fill_n(window_[index(side)], ladder_window, LevelRef{0});
        }
    }
}

template <Dispatch D>
std::size_t BasicOrderBook<D>::slot(Side side, Price price) const noexcept
{
    const std::uint64_t offset = static_cast<std::uint64_t>(price.ticks) - low_[index(side)];
    if (offset >= ladder_window * step_) {
        return ladder_window;
    }
    const std::uint64_t at = offset / step_;
    return at * step_ == offset ? static_cast<std::size_t>(at) : ladder_window;
}

template <Dispatch D>
bool BasicOrderBook<D>::central(Side side, Price price) const noexcept
{
    const std::uint64_t at = (static_cast<std::uint64_t>(price.ticks) - low_[index(side)]) / step_;
    return at >= ladder_window / 4 && at < ladder_window * 3 / 4;
}

// Centres the window on the best, its slots on whole steps, and sets the slot of each level inside it on the
// grid: O(ladder_window + levels inside).
template <Dispatch D>
void BasicOrderBook<D>::recentre(Side side) noexcept
{
    const auto step = static_cast<std::int64_t>(step_);
    const std::int64_t best = best_[index(side)]->price().ticks;
    const std::int64_t grid = best - ((best % step) + step) % step;  // the step at or below the best
    low_[index(side)] = static_cast<std::uint64_t>(grid - step * static_cast<std::int64_t>(ladder_window / 2));

    LevelRef* window = window_[index(side)];
    std::fill_n(window, ladder_window, LevelRef{0});
    for (const PriceLevel* level = best_[index(side)]; level != nullptr; level = level->worse_) {
        const std::uint64_t offset = static_cast<std::uint64_t>(level->price().ticks) - low_[index(side)];
        if (offset >= ladder_window * step_) {
            break;  // every level after it is further from the best
        }
        if (const std::size_t at = slot(side, level->price()); at < ladder_window) {
            window[at] = storage_.levels.ref(*level);
        }
    }
}

template <Dispatch D>
PriceLevel* BasicOrderBook<D>::find_level(Side side, Price price) const noexcept
{
    if (ladder()) {
        if (const std::size_t at = slot(side, price); at < ladder_window) {
            const LevelRef ref = window_[index(side)][at];
            return ref != 0 ? &storage_.levels.at(ref) : nullptr;
        }
    }
    const LevelRef ref = storage_.level_index.find(symbol_, side, price);
    return ref != 0 ? &storage_.levels.at(ref) : nullptr;
}

// Links a new level after every level better than it. The list walks from the best, O(levels passed); the
// ladder, for a level on its grid behind the best, first scans from the level's slot towards the best for the
// nearest slot set, then walks past any level off the grid between them.
// ponytail: the list's walk is its largest cost on the full day, which the ladder skips inside its window; the
// scan reads up to ladder_window slots, where a bitmap of the slots set would find the nearest in a few words.
template <Dispatch D>
PriceLevel& BasicOrderBook<D>::new_level(Side side, Price price) noexcept
{
    PriceLevel& level = *storage_.levels.acquire(price);  // room() saw a free slot
    const LevelRef ref = storage_.levels.ref(level);
    [[maybe_unused]] const bool indexed = storage_.level_index.add(symbol_, side, price, ref);
    assert(indexed);

    PriceLevel*& best = best_[index(side)];
    PriceLevel* before = nullptr;
    const std::size_t at = ladder() ? slot(side, price) : ladder_window;
    if (at < ladder_window && best != nullptr && better(side, best->price(), price)) {
        const LevelRef* window = window_[index(side)];
        if (side == Side::Buy) {
            for (std::size_t i = at + 1; i < ladder_window && before == nullptr; ++i) {
                before = window[i] != 0 ? &storage_.levels.at(window[i]) : nullptr;
            }
        } else {
            for (std::size_t i = at; i > 0 && before == nullptr; --i) {
                before = window[i - 1] != 0 ? &storage_.levels.at(window[i - 1]) : nullptr;
            }
        }
    }
    PriceLevel* after = before != nullptr ? before->worse_ : best;
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

    if (ladder()) {
        if (at < ladder_window) {
            window_[index(side)][at] = ref;
        }
        if (!central(side, best->price())) {
            recentre(side);  // a side's first level is centred here too, unless already central
        }
    }
    return level;
}

template <Dispatch D>
void BasicOrderBook<D>::drop_level(Side side, PriceLevel& level) noexcept
{
    if (ladder()) {
        if (const std::size_t at = slot(side, level.price()); at < ladder_window) {
            window_[index(side)][at] = 0;
        }
    }

    PriceLevel*& best = best_[index(side)];
    (level.better_ != nullptr ? level.better_->worse_ : best) = level.worse_;
    if (level.worse_ != nullptr) {
        level.worse_->better_ = level.better_;
    }
    level.better_ = nullptr;
    level.worse_ = nullptr;
    [[maybe_unused]] const bool erased = storage_.level_index.erase(symbol_, side, level.price());
    assert(erased);
    storage_.levels.release(level);

    if (ladder() && best != nullptr && !central(side, best->price())) {
        recentre(side);
    }
}

// Leaves the order's state to the caller.
template <Dispatch D>
bool BasicOrderBook<D>::remove(Order& order) noexcept
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

template <Dispatch D>
BookStructure BasicOrderBook<D>::room(Side side, Price price, const Order* leaving) const noexcept
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

template <Dispatch D>
bool BasicOrderBook<D>::add(Order& order) noexcept
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

template <Dispatch D>
bool BasicOrderBook<D>::cancel(OrderId id) noexcept
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

template <Dispatch D>
Order* BasicOrderBook<D>::find(OrderId id) noexcept
{
    return storage_.order_index.find(symbol_, id);
}

template <Dispatch D>
const Order* BasicOrderBook<D>::find(OrderId id) const noexcept
{
    return storage_.order_index.find(symbol_, id);
}

template <Dispatch D>
const Order* BasicOrderBook<D>::best_bid() const noexcept
{
    const PriceLevel* level = best(Side::Buy);
    return level != nullptr ? level->front() : nullptr;
}

template <Dispatch D>
const Order* BasicOrderBook<D>::best_ask() const noexcept
{
    const PriceLevel* level = best(Side::Sell);
    return level != nullptr ? level->front() : nullptr;
}

template <Dispatch D>
bool BasicOrderBook<D>::empty() const noexcept
{
    return size_ == 0;
}

template <Dispatch D>
std::size_t BasicOrderBook<D>::level_count() const noexcept
{
    std::size_t count = 0;
    for (const PriceLevel* first : best_) {
        for (const PriceLevel* level = first; level != nullptr; level = level->worse_) {
            ++count;
        }
    }
    return count;
}

template <Dispatch D>
bool BasicOrderBook<D>::windows_hold() const noexcept
{
    if (!ladder()) {
        return true;
    }
    for (const Side side : {Side::Buy, Side::Sell}) {
        const LevelRef* window = window_[index(side)];
        std::size_t inside = 0;
        for (const PriceLevel* level = best(side); level != nullptr; level = level->worse_) {
            if (const std::size_t at = slot(side, level->price()); at < ladder_window) {
                if (window[at] != storage_.levels.ref(*level)) {
                    return false;
                }
                ++inside;
            }
        }
        if (std::count_if(window, window + ladder_window, [](LevelRef ref) { return ref != 0; }) !=
            static_cast<std::ptrdiff_t>(inside)) {
            return false;
        }
        if (best(side) != nullptr && !central(side, best(side)->price())) {
            return false;
        }
    }
    return true;
}

template <Dispatch D>
bool BasicOrderBook<D>::execute(OrderId id, Quantity quantity) noexcept
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

template <Dispatch D>
bool BasicOrderBook<D>::cancel(OrderId id, Quantity quantity) noexcept
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

template <Dispatch D>
bool BasicOrderBook<D>::erase(OrderId id) noexcept
{
    Order* order = find(id);

    if (order == nullptr) {
        return false;
    }

    return remove(*order);
}

template class BasicOrderBook<Dispatch::Selected>;
template class BasicOrderBook<Dispatch::List>;
template class BasicOrderBook<Dispatch::Ladder>;

} // namespace simulator
