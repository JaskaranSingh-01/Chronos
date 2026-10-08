#pragma once

#include <cstdint>
#include <span>

#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_message.hpp"

namespace simulator {

enum class ItchReadResult : std::uint8_t {
    Message,
    EndOfBuffer,
    Error
};

enum class ItchReadError : std::uint8_t {
    None,
    MissingLength,
    InvalidLength,
    TruncatedMessage
};

class ItchMessageReader
{
private:
    ByteReader reader_;
    ItchReadError error_{ItchReadError::None};

public:
    explicit ItchMessageReader(std::span<const std::byte> data) noexcept : reader_(data){}

    ItchReadResult next(ItchMessage& message) noexcept;

    constexpr ItchReadError error() const noexcept{
        return error_;
    }

    constexpr std::size_t position() const noexcept{
        return reader_.position();
    }

    constexpr std::size_t remaining() const noexcept{
        return reader_.remaining();
    }
};

} // namespace simulator