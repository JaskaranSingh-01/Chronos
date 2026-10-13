#include "alloc_counter.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::uint64_t> allocations{0};

void* allocate(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc{};
}

void* allocate(std::size_t size, std::align_val_t align)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (::posix_memalign(&p, std::max(static_cast<std::size_t>(align), sizeof(void*)), size != 0 ? size : 1) == 0) {
        return p;
    }
    throw std::bad_alloc{};
}

} // namespace

std::uint64_t simulator::allocation_count() noexcept
{
    return allocations.load(std::memory_order_relaxed);
}

// The nothrow forms are left to the library, whose defaults call these.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t align) { return allocate(size, align); }
void* operator new[](std::size_t size, std::align_val_t align) { return allocate(size, align); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
