#include "simulator/itch/itch_message_reader.hpp"

namespace simulator {

ItchReadResult ItchMessageReader::next(
    ItchMessage& message
) noexcept
{
    message = ItchMessage{};
    error_ = ItchReadError::None;

    if (reader_.remaining() == 0) {
        return ItchReadResult::EndOfBuffer;
    }

    /*
     * Work on a temporary reader.
     *
     * If the message is malformed or truncated,
     * the real reader position is not changed.
     */
    ByteReader candidate = reader_;

    std::uint16_t message_length{0};

    if (!candidate.read_u16_be(message_length)) {
        error_ = ItchReadError::MissingLength;
        return ItchReadResult::Error;
    }

    /*
     * The length includes the message-type byte.
     *
     * Therefore a message must contain at least
     * one byte for the message type.
     */
    if (message_length == 0) {
        error_ = ItchReadError::InvalidLength;
        return ItchReadResult::Error;
    }

    if (candidate.remaining() < message_length) {
        error_ = ItchReadError::TruncatedMessage;
        return ItchReadResult::Error;
    }

    std::span<const std::byte> message_bytes;

    if (!candidate.read_bytes(
            message_length,
            message_bytes))
    {
        error_ = ItchReadError::TruncatedMessage;
        return ItchReadResult::Error;
    }

    /*
     * First byte is the ITCH message type.
     */
    message.type =
        std::to_integer<std::uint8_t>(
            message_bytes[0]
        );

    /*
     * Everything after the type byte is the
     * message-specific payload.
     */
    message.payload =
        message_bytes.subspan(1);

    /*
     * Commit the reader position only after the
     * complete message has been validated.
     */
    reader_ = candidate;

    return ItchReadResult::Message;
}

} // namespace simulator