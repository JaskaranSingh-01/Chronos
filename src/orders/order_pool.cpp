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

    // Build the free-list.
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

    // Verify that the Order belongs to this pool.
    Order* first = &storage_[0].order;
    Order* last = &storage_[capacity_ - 1].order;

    if (&order < first || &order > last) {
        return false;
    }

    // Check that the order is aligned with one of our slots.
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

    // An order must not still belong to a PriceLevel.
    //
    // We cannot access level_ directly here because it is
    // private to Order. For now, enforce this through state:
    // the caller must remove the order from OrderBook first.
    //
    // The stronger ownership check will be added when we
    // integrate the pool with OrderBook.

    slot->next = free_list_;
    free_list_ = slot;

    ++available_;

    return true;
}

} // namespace simulator