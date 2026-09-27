#pragma once
#include "simulator/types/timestamp.hpp"

namespace simulator {
class Clock {
private:
  Timestamp current_time_{0};

public:
  constexpr Timestamp now() const noexcept { return current_time_; }
  constexpr void advance_to(Timestamp timestamp) noexcept {
    current_time_ = timestamp;
  }
};
} // namespace simulator