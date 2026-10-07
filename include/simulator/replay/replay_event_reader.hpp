#pragma once

#include "simulator/replay/replay_event.hpp"

namespace simulator {

enum class ReplayReadResult
{
    Event,
    EndOfFile,
    Error
};

class ReplayEventReader
{
public:
    virtual ~ReplayEventReader() = default;

    virtual ReplayReadResult next(ReplayEvent& event) = 0;
    virtual std::size_t source_position() const noexcept = 0;
};

} // namespace simulator