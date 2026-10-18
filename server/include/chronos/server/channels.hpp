#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace simulator::server {

// A first-in first-out queue of fixed capacity; not thread-safe. Allocates only in its constructor.
template <class T>
class FixedQueue
{
private:
    std::vector<T> items_;
    std::size_t head_{0};
    std::size_t size_{0};

public:
    explicit FixedQueue(std::size_t capacity)
        : items_(capacity)
    {}

    // False, changing nothing, when full.
    bool push(const T& item) noexcept
    {
        if (size_ == items_.size()) {
            return false;
        }
        items_[(head_ + size_) % items_.size()] = item;
        ++size_;
        return true;
    }

    // False when empty.
    bool pop(T& item) noexcept
    {
        if (size_ == 0) {
            return false;
        }
        item = items_[head_];
        head_ = (head_ + 1) % items_.size();
        --size_;
        return true;
    }

    std::size_t size() const noexcept
    {
        return size_;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }
};

// The latest value published. The writer never waits: while a reader holds the slot, a publish is dropped.
template <class T>
class LatestSlot
{
private:
    mutable std::mutex mutex_;
    T value_{};
    bool published_{false};

public:
    // False, dropping `value`, while a reader holds the slot.
    bool try_publish(const T& value) noexcept
    {
        const std::unique_lock lock{mutex_, std::try_to_lock};
        if (!lock.owns_lock()) {
            return false;
        }
        value_ = value;
        published_ = true;
        return true;
    }

    // Calls f(const T&) while holding the slot; false, without calling f, before the first publish.
    template <class F>
    bool read(F&& f) const
    {
        const std::lock_guard lock{mutex_};
        if (!published_) {
            return false;
        }
        f(value_);
        return true;
    }
};

// The last `capacity` records, numbered from 1; a reader more than `capacity` behind finds a gap.
// ponytail: one mutex for writers and readers (a read copies at most out.size()); seqlock ring if the engine waits.
template <class T>
class RecordRing
{
private:
    mutable std::mutex mutex_;
    std::vector<T> records_;
    std::uint64_t next_seq_{1};

public:
    struct Read
    {
        std::size_t count{0};        // records copied
        // The oldest still held; records before it were overwritten, so a gap is max(from, 1) < first_seq.
        std::uint64_t first_seq{1};
        std::uint64_t last_seq{0};   // the newest; 0 before the first
    };

    explicit RecordRing(std::size_t capacity)
        : records_(capacity)
    {}

    // Numbers `record` (its seq member), stores it over the oldest once full, and returns the seq.
    std::uint64_t publish(T record) noexcept
    {
        const std::lock_guard lock{mutex_};
        record.seq = next_seq_++;
        records_[record.seq % records_.size()] = record;
        return record.seq;
    }

    // Copies the records from max(from_seq, first_seq) on into `out`, oldest first.
    Read read(std::uint64_t from_seq, std::span<T> out) const noexcept
    {
        const std::lock_guard lock{mutex_};
        Read result{.last_seq = next_seq_ - 1};
        result.first_seq = next_seq_ > records_.size() ? next_seq_ - records_.size() : 1;
        for (std::uint64_t seq = std::max(from_seq, result.first_seq);
             seq <= result.last_seq && result.count < out.size(); ++seq) {
            out[result.count++] = records_[seq % records_.size()];
        }
        return result;
    }

    std::uint64_t last_seq() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return next_seq_ - 1;
    }
};

}  // namespace simulator::server
