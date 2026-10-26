#pragma once

#include <cstdint>

namespace simulator {

// Global operator new and new[] calls since start-up; always 0 in ASan builds, where ASan owns operator new.
// Defined in alloc_counter.cpp, which replaces those operators and is linked into executables only.
std::uint64_t allocation_count() noexcept;

// What the process's heap holds now, across its malloc zones: it sees a malloc, or a library's, that the count above
// cannot. glibc counts no blocks, so blocks is 0 on Linux.
struct HeapUsage
{
    std::uint64_t blocks{0};
    std::uint64_t bytes{0};
};

HeapUsage heap_usage() noexcept;

} // namespace simulator
