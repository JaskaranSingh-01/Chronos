#include "simulator/orders/order_index.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <utility>

namespace simulator {

namespace {

constexpr std::size_t min_slots = 16;

} // namespace

OrderIndex::OrderIndex(std::size_t capacity)
{
    if (capacity != 0) {
        rehash(std::bit_ceil(std::max(min_slots, 2 * capacity)));
    }
}

void OrderIndex::rehash(std::size_t slot_count)
{
    const std::unique_ptr<Slot[]> old = std::exchange(slots_, std::make_unique<Slot[]>(slot_count));
    const std::size_t old_count = old ? mask_ + 1 : 0;
    mask_ = slot_count - 1;
    shift_ = 64 - std::countr_zero(slot_count);

    for (std::size_t i = 0; i < old_count; ++i) {
        if (old[i].order != nullptr) {
            std::size_t j = home(old[i].id);
            while (slots_[j].order != nullptr) {
                j = (j + 1) & mask_;
            }
            slots_[j] = old[i];
        }
    }
}

bool OrderIndex::insert(OrderId id, Order* order)
{
    assert(order != nullptr);  // nullptr marks a free slot

    if (size_ == capacity()) {
        rehash(slots_ ? 2 * (mask_ + 1) : min_slots);
    }

    std::size_t i = home(id);
    for (; slots_[i].order != nullptr; i = (i + 1) & mask_) {
        if (slots_[i].id == id) {
            return false;
        }
    }
    slots_[i] = {id, order};
    ++size_;
    return true;
}

bool OrderIndex::erase(OrderId id) noexcept
{
    if (size_ == 0) {
        return false;
    }

    std::size_t hole = home(id);
    for (;; hole = (hole + 1) & mask_) {
        if (slots_[hole].order == nullptr) {
            return false;
        }
        if (slots_[hole].id == id) {
            break;
        }
    }

    // Backward shift: each later entry of the run moves into the hole unless
    // its home lies after the hole, where its lookups never pass the hole.
    for (std::size_t next = (hole + 1) & mask_; slots_[next].order != nullptr; next = (next + 1) & mask_) {
        if (((next - home(slots_[next].id)) & mask_) >= ((next - hole) & mask_)) {
            slots_[hole] = slots_[next];
            hole = next;
        }
    }
    slots_[hole] = Slot{};
    --size_;
    return true;
}

} // namespace simulator
