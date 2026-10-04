#pragma once

#include <cstdint>

#include "simulator/itch/itch_message.hpp"
#include "simulator/itch/itch_messages.hpp"

namespace simulator {

enum class ItchDecodeResult : std::uint8_t {
    Success,
    UnsupportedMessage,
    InvalidMessage
};

enum class ItchDecodeError : std::uint8_t {
    None,
    InvalidMessageType,
    InvalidPayloadLength,
    InvalidSide,
    TruncatedPayload
};

class ItchDecoder
{
private:
    ItchDecodeError error_{
        ItchDecodeError::None
    };

public:
    ItchDecodeResult decode_add_order(
        const ItchMessage& message,
        ItchAddOrder& order
    ) noexcept;

    constexpr ItchDecodeError error() const noexcept
    {
        return error_;
    }
};

} // namespace simulator