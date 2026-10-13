#pragma once

#include <cstdint>

namespace simulator {

// Global operator new and new[] calls since start-up. Defined in alloc_counter.cpp,
// which replaces those operators and is linked into executables only.
std::uint64_t allocation_count() noexcept;

} // namespace simulator
