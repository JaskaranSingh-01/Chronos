#pragma once

#include <cstddef>
#include <span>

#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/itch/itch_normalizer.hpp"
#include "simulator/replay/replay_event_reader.hpp"

namespace simulator {

class ItchReplayReader : public ReplayEventReader
{
private:
    ItchMessageReader message_reader_;
    ItchDecoder decoder_;
    ItchNormalizer normalizer_;

public:
    explicit ItchReplayReader(std::span<const std::byte> data) noexcept : message_reader_(data){}

    ReplayReadResult next(ReplayEvent& event) noexcept override;

    std::size_t position() const noexcept{
        return message_reader_.position();
    }

    std::size_t remaining() const noexcept{
        return message_reader_.remaining();
    }

    const ItchMessageReader& message_reader() const noexcept{
        return message_reader_;
    }
    std::size_t source_position() const noexcept override{
        return message_reader_.position();
    }
};

} // namespace simulator