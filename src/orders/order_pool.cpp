#include "simulator/orders/order_pool.hpp"

#include <cassert>

namespace simulator {

OrderPool::OrderPool(std::size_t capacity)
    : storage_(capacity > 0 ? std::make_unique<Slot[]>(capacity) : nullptr),
      free_list_(nullptr),
      capacity_(capacity),
      available_(capacity)
{
    if (capacity == 0) {
        return;
    }

    for (std::size_t i = 0; i < capacity_; ++i) {
        storage_[i].next =
            (i + 1 < capacity_)
                ? &storage_[i + 1]
                : nullptr;
    }

    free_list_ = &storage_[0];
}


Order* OrderPool::acquire(
    OrderId id,
    Side side,
    Price price,
    Quantity quantity,
    Timestamp timestamp
) noexcept
{
    if (free_list_ == nullptr) {
        return nullptr;
    }

    Slot* slot = free_list_;

    free_list_ = slot->next;

    slot->next = nullptr;

    assert(slot->is_available);
    slot->is_available = false;

    --available_;

    slot->order.initialize(
        id,
        side,
        price,
        quantity,
        timestamp
    );

    return &slot->order;
}


bool OrderPool::release(Order& order) noexcept
{
    if (capacity_ == 0) {
        return false;
    }

    // Reject an order outside this pool or not at a slot boundary.
    Order* first = &storage_[0].order;
    Order* last = &storage_[capacity_ - 1].order;

    if (&order < first || &order > last) {
        return false;
    }

    const auto address =
        reinterpret_cast<std::uintptr_t>(&order);

    const auto first_address =
        reinterpret_cast<std::uintptr_t>(first);

    const auto slot_size =
        sizeof(Slot);

    if ((address - first_address) % slot_size != 0) {
        return false;
    }

    Slot* slot =
        reinterpret_cast<Slot*>(
            reinterpret_cast<char*>(&order) -
            offsetof(Slot, order)
        );

    if (slot->is_available) {
        return false;
    }
    
    // A linked order is still referenced by its PriceLevel.
    if (order.level_ != nullptr){
        return false;
    }

    slot->next = free_list_;
    free_list_ = slot;
    slot->is_available = true;

    ++available_;

    return true;
}

} // namespace simulator
