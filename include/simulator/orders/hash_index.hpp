#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace simulator {

// Linear probing over a power-of-two table of at least twice `capacity` slots, sized once: it never allocates after
// its constructor, and a probe always ends at a free slot. erase() back-shifts the probe run, so there are no
// tombstones and a day of adds and erases never lengthens a probe. Slot{} is a free slot; slot.empty() says so and
// slot.hash() is its key's hash.
template <class Slot>
class HashIndex
{
private:
    std::unique_ptr<Slot[]> slots_;
    std::size_t mask_{0};  // slot count - 1
    int shift_{64};        // 64 - log2(slot count)
    std::size_t size_{0};

public:
    explicit HashIndex(std::size_t capacity)
    {
        const std::size_t slots = std::bit_ceil(std::max<std::size_t>(16, 2 * capacity));
        // ponytail: value-initialised, so every page is touched up front; mmap zero pages if resident memory matters.
        slots_ = std::make_unique<Slot[]>(slots);
        mask_ = slots - 1;
        shift_ = 64 - std::countr_zero(slots);
    }

    // Fibonacci hashing: the top bits of hash * 2^64 / phi spread sequential keys evenly. Public for tests.
    std::size_t home(std::uint64_t hash) const noexcept
    {
        return static_cast<std::size_t>((hash * 0x9E37'79B9'7F4A'7C15ULL) >> shift_);
    }

    // The slot of `hash` that `match` accepts; nullptr if none.
    template <class Match>
    const Slot* find(std::uint64_t hash, Match match) const noexcept
    {
        for (std::size_t i = home(hash);; i = (i + 1) & mask_) {
            if (slots_[i].empty()) {
                return nullptr;
            }
            if (match(slots_[i])) {
                return &slots_[i];
            }
        }
    }

    // False, changing nothing, if full. The caller knows the key is absent.
    [[nodiscard]] bool add(const Slot& entry) noexcept
    {
        if (full()) {
            return false;
        }
        std::size_t i = home(entry.hash());
        while (!slots_[i].empty()) {
            i = (i + 1) & mask_;
        }
        slots_[i] = entry;
        ++size_;
        return true;
    }

    // False if no slot of `hash` matches.
    template <class Match>
    bool erase(std::uint64_t hash, Match match) noexcept
    {
        std::size_t hole = home(hash);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].empty()) {
                return false;
            }
            if (match(slots_[hole])) {
                break;
            }
        }

        // Backward shift: each later entry of the run moves into the hole unless
        // its home lies after the hole, where its lookups never pass the hole.
        for (std::size_t next = (hole + 1) & mask_; !slots_[next].empty(); next = (next + 1) & mask_) {
            if (((next - home(slots_[next].hash())) & mask_) >= ((next - hole) & mask_)) {
                slots_[hole] = slots_[next];
                hole = next;
            }
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    // Half its slots, so a probe run stays short.
    std::size_t capacity() const noexcept
    {
        return (mask_ + 1) / 2;
    }

    bool full() const noexcept
    {
        return size_ == capacity();
    }

    std::size_t bytes() const noexcept
    {
        return (mask_ + 1) * sizeof(Slot);
    }
};

}  // namespace simulator
