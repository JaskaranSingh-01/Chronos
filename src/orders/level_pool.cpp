#include "simulator/orders/level_pool.hpp"

#include <cassert>
#include <type_traits>

namespace simulator {

// So make_unique_for_overwrite leaves the slots untouched until they are handed out.
static_assert(std::is_trivially_default_constructible_v<PriceLevel>);

LevelPool::LevelPool(std::size_t capacity)
    : slots_(std::make_unique_for_overwrite<PriceLevel[]>(capacity + 1)), capacity_(capacity)
{
    assert(capacity < (std::size_t{1} << 31));
}

PriceLevel* LevelPool::acquire(Price price) noexcept
{
    PriceLevel* level = free_;
    if (level != nullptr) {
        free_ = level->worse_;
    } else if (next_ <= capacity_) {
        level = &slots_[next_++];
    } else {
        return nullptr;
    }
    *level = PriceLevel{price};
    ++in_use_;
    return level;
}

void LevelPool::release(PriceLevel& level) noexcept
{
    assert(level.empty() && level.better_ == nullptr && level.worse_ == nullptr && in_use_ != 0);
    level.worse_ = free_;
    free_ = &level;
    --in_use_;
}

} // namespace simulator
