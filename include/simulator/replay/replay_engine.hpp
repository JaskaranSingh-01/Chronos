#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "simulator/replay/replay_event.hpp"
#include "simulator/replay/replay_event_reader.hpp"
#include "simulator/simulator.hpp"

namespace simulator {

enum class ReplayFailure : std::uint8_t {
    None,
    ReaderError,
    OutOfOrderTimestamp
};

struct ReplayStatistics {
    std::size_t received_events{0};
    std::size_t processed_events{0};
    std::size_t add_events{0};
    std::size_t cancel_events{0};
    std::size_t rejected_events{0};
    std::size_t trades{0};
    std::size_t execute_events{0};
    std::size_t delete_events{0};
};

struct ReplayResult {
    ReplayStatistics statistics{};
    ReplayFailure failure{ReplayFailure::None};

    // One-based logical event position; zero means no failure.
    std::size_t failed_event_index{0};

    // CSV source line when replaying through ReplayEventReader.
    std::size_t failed_source_line{0};

    constexpr bool succeeded() const noexcept
    {
        return failure == ReplayFailure::None;
    }
};

class ReplayEngine
{
private:
    enum class ProcessResult {
        Processed,
        Rejected
    };

    Simulator& simulator_;

    ProcessResult process_event(
        const ReplayEvent& event,
        ReplayStatistics& statistics
    );

public:
    explicit ReplayEngine(Simulator& simulator) noexcept
        : simulator_(simulator)
    {}

    ReplayResult replay(
        const std::vector<ReplayEvent>& events
    );

    ReplayResult replay(
        ReplayEventReader& reader
    );
};

} // namespace simulator