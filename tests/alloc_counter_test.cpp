#include <gtest/gtest.h>

#include <cstdint>
#include <new>
#include <vector>

#include "alloc_counter.hpp"

namespace simulator {
namespace {

const void* volatile sink;

// Escapes through a volatile store, so the optimiser cannot drop the allocation.
template <class T>
void keep(T* pointer)
{
    sink = pointer;
}

TEST(AllocationCounterTest, NewIntCountsExactlyOne)
{
    const std::uint64_t before = allocation_count();
    int* value = new int{7};
    keep(value);
    const std::uint64_t after = allocation_count();
    delete value;

    EXPECT_EQ(after - before, 1u);
}

TEST(AllocationCounterTest, ArrayAndAlignedNewCount)
{
    struct alignas(64) Line
    {
        char bytes[64];
    };

    const std::uint64_t before = allocation_count();
    int* values = new int[16];
    Line* line = new Line{};
    keep(values);
    keep(line);
    const std::uint64_t after = allocation_count();
    delete[] values;
    delete line;

    EXPECT_EQ(after - before, 2u);
}

TEST(AllocationCounterTest, CountsContainerAllocations)
{
    std::vector<int> values;
    const std::uint64_t before = allocation_count();
    values.reserve(1'000);
    keep(values.data());
    EXPECT_EQ(allocation_count() - before, 1u);
}

} // namespace
} // namespace simulator
