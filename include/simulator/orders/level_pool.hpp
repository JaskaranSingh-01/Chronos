#pragma once

#include <cstddef>
#include <memory>

#include "simulator/orders/price_level.hpp"

namespace simulator {

// Fixed-capacity PriceLevel storage with a free list. It allocates only in its constructor and writes a slot only when
// it first hands it out, so its memory follows the most levels ever live, not its capacity. Slot 0 is never handed
// out: a LevelRef of 0 is none.
class LevelPool
{
private:
    std::unique_ptr<PriceLevel[]> slots_;  // capacity + 1, uninitialized until handed out
    std::size_t capacity_;
    std::size_t next_{1};        // the first slot never handed out; it and those after it are untouched
    std::size_t in_use_{0};
    PriceLevel* free_{nullptr};  // released slots, linked through worse_

public:
    // At most 2^31 - 1, so a LevelRef shifted left one bit fits 32 bits.
    explicit LevelPool(std::size_t capacity);

    LevelPool(const LevelPool&) = delete;
    LevelPool& operator=(const LevelPool&) = delete;

    // An empty, unlinked level at `price`; nullptr when every slot is in use.
    PriceLevel* acquire(Price price) noexcept;

    // `level` is empty, unlinked and in use.
    void release(PriceLevel& level) noexcept;

    LevelRef ref(const PriceLevel& level) const noexcept
    {
        return static_cast<LevelRef>(&level - slots_.get());
    }

    PriceLevel& at(LevelRef ref) noexcept
    {
        return slots_[ref];
    }

    const PriceLevel& at(LevelRef ref) const noexcept
    {
        return slots_[ref];
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    std::size_t in_use() const noexcept
    {
        return in_use_;
    }

    bool full() const noexcept
    {
        return in_use_ == capacity_;
    }

    std::size_t bytes() const noexcept
    {
        return (capacity_ + 1) * sizeof(PriceLevel);
    }
};

} // namespace simulator
