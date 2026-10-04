#pragma once

#include <cstddef>
#include <vector>

#include "simulator/replay/replay_event.hpp"
#include "simulator/replay/replay_event_reader.hpp"
#include "simulator/simulator.hpp"

namespace simulator {

class ReplayEngine
{
private:
    Simulator& simulator_;

    std::size_t process_event(
        const ReplayEvent& event
    );

public:
    explicit ReplayEngine(Simulator& simulator) noexcept
        : simulator_(simulator)
    {}

    std::size_t replay(
        const std::vector<ReplayEvent>& events
    );

    std::size_t replay(
        ReplayEventReader& reader
    );
};

} // namespace simulator