#pragma once

#include <cstdint>

#include "simulator/itch/itch_messages.hpp"
#include "simulator/replay/replay_event.hpp"

namespace simulator {

class ItchNormalizer
{
public:
    ReplayEvent normalize(
        const ItchAddOrder& order
    ) const noexcept;
};

} // namespace simulator