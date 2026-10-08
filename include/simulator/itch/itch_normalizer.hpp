#pragma once

#include "simulator/itch/itch_messages.hpp"
#include "simulator/replay/replay_event.hpp"

namespace simulator {

class ItchNormalizer
{
public:
    ReplayEvent normalize(const ItchAddOrder& order) const noexcept;
    ReplayEvent normalize(const ItchOrderExecuted& order) const noexcept;
    ReplayEvent normalize(const ItchOrderCancel& order) const noexcept;
    ReplayEvent normalize(const ItchOrderDelete& order) const noexcept;
    ReplayEvent normalize(const ItchOrderReplace& order) const noexcept;
};

} // namespace simulator