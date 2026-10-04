#pragma once

#include <cstddef>
#include <fstream>
#include <string>

#include "simulator/replay/replay_event.hpp"

namespace simulator {

enum class ReplayReadResult {
    Event,
    EndOfFile,
    Error
};

class ReplayEventReader
{
private:
    std::ifstream file_;
    std::string error_;

    bool header_read_{false};
    std::size_t line_number_{0};

    bool parse_event(
        const std::string& line,
        ReplayEvent& event
    );

public:
    explicit ReplayEventReader(
        const std::string& filename
    );

    ReplayReadResult next(
        ReplayEvent& event
    );

    bool is_open() const noexcept {
        return file_.is_open();
    }

    const std::string& error() const noexcept {
        return error_;
    }

    std::size_t line_number() const noexcept {
        return line_number_;
    }
};

} // namespace simulator
