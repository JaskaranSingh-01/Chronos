#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "simulator/types/order_id.hpp"

namespace simulator {

class Order;

// OrderId -> Order*: linear probing over a power-of-two table at most half full.
// erase() back-shifts the probe run, so there are no tombstones. Allocates only to grow.
class OrderIndex
{
private:
    struct Slot
    {
        OrderId id{0};
        Order* order{nullptr};  // nullptr: a free slot
    };

    std::unique_ptr<Slot[]> slots_;
    std::size_t mask_{0};  // slot count - 1
    int shift_{64};        // 64 - log2(slot count)
    std::size_t size_{0};

    void rehash(std::size_t slot_count);

public:
    // Room for `capacity` orders before the first growth; 0 allocates on the first insert().
    explicit OrderIndex(std::size_t capacity = 0);

    // Fibonacci hashing (top bits of id * 2^64 / phi) spreads sequential ids evenly.
    // Public for tests.
    std::size_t home(OrderId id) const noexcept
    {
        return static_cast<std::size_t>((id * 0x9E37'79B9'7F4A'7C15ULL) >> shift_);
    }

    // nullptr if `id` is absent.
    Order* find(OrderId id) const noexcept
    {
        if (size_ == 0) {
            return nullptr;
        }
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Slot& slot = slots_[i];
            if (slot.order == nullptr) {
                return nullptr;
            }
            if (slot.id == id) {
                return slot.order;
            }
        }
    }

    // False if `id` is present. Doubles the table first if it is full (size() == capacity()).
    bool insert(OrderId id, Order* order);

    // False if `id` is absent.
    bool erase(OrderId id) noexcept;

    std::size_t size() const noexcept
    {
        return size_;
    }

    // Orders it holds before it grows.
    std::size_t capacity() const noexcept
    {
        return slots_ ? (mask_ + 1) / 2 : 0;
    }
};

} // namespace simulator
