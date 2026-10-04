#pragma once

#include <cstddef>
#include <vector>

#include "simulator/replay/replay_event.hpp"
#include "simulator/simulator.hpp"

namespace simulator {

class ReplayEngine
{
private:
    Simulator& simulator_;

public:
    explicit ReplayEngine(Simulator& simulator) noexcept
        : simulator_(simulator)
    {}

    std::size_t replay(
        const std::vector<ReplayEvent>& events
    );
};

} // namespace simulator