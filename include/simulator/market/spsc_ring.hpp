#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace simulator {

// One producer thread hands values to one consumer thread through a fixed power-of-two array (spec §4.2); nothing
// allocates after the constructor. A push writes its slot, then publishes it with a store-release of the write index;
// a pop load-acquires the write index before it reads the slot, then frees the slot with a store-release of the read
// index, which a push load-acquires before it reuses the slot. Each index sits on a cache line of its own, with its
// owner's copy of the other's last seen value, so a side reads the other's line only when its copy says full or
// empty.
template <class T>
class SpscRing
{
private:
    static constexpr std::size_t line = 128;  // a cache line on Apple silicon; two on x86-64

    std::unique_ptr<T[]> slots_;
    std::uint64_t mask_;
    alignas(line) std::atomic<std::uint64_t> write_{0};  // the producer's
    std::uint64_t read_seen_{0};                         // the producer's copy of read_
    alignas(line) std::atomic<std::uint64_t> read_{0};   // the consumer's
    std::uint64_t write_seen_{0};                        // the consumer's copy of write_

public:
    // `capacity` is a power of two.
    explicit SpscRing(std::size_t capacity)
        : slots_(std::make_unique<T[]>(capacity)), mask_(capacity - 1)
    {
        assert(std::has_single_bit(capacity));
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // The producer's: false, changing nothing, if the ring is full.
    bool try_push(const T& value) noexcept
    {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        if (write - read_seen_ > mask_) {
            read_seen_ = read_.load(std::memory_order_acquire);
            if (write - read_seen_ > mask_) {
                return false;
            }
        }
        slots_[write & mask_] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    // The consumer's: false if the ring is empty.
    bool try_pop(T& value) noexcept
    {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        if (read == write_seen_) {
            write_seen_ = write_.load(std::memory_order_acquire);
            if (read == write_seen_) {
                return false;
            }
        }
        value = slots_[read & mask_];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept
    {
        return static_cast<std::size_t>(mask_ + 1);
    }

    std::size_t bytes() const noexcept
    {
        return capacity() * sizeof(T);
    }
};

} // namespace simulator
