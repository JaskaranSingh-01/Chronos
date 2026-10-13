#pragma once

#include <cstdint>

namespace simulator {

// Calls to the global operator new and new[] (plain and aligned) since start-up.
// Defined by app/alloc_counter.cpp, which replaces those operators: link it into
// an executable (simulator, alloc_counter_test), never into simulator_lib.
std::uint64_t allocation_count() noexcept;

} // namespace simulator
