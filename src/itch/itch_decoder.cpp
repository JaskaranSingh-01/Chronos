#include "simulator/itch/itch_decoder.hpp"

#include <cstddef>
#include <cstdint>

#include "simulator/itch/byte_reader.hpp"

namespace simulator {

namespace {

constexpr std::uint8_t AddOrderMessageType = 0x41; // 'A'

constexpr std::size_t AddOrderPayloadSize = 31;

bool parse_side(
    std::uint8_t value,
    Side& side
) noexcept
{
    if (value == 'B') {
        side = Side::Buy;
        return true;
    }

    if (value == 'S') {
        side = Side::Sell;
        return true;
    }

    return false;
}

} // namespace

ItchDecodeResult ItchDecoder::decode_add_order(
    const ItchMessage& message,
    ItchAddOrder& order
) noexcept
{
    error_ = ItchDecodeError::None;
    order = ItchAddOrder{};

    if (message.type != AddOrderMessageType) {
        error_ = ItchDecodeError::InvalidMessageType;

        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != AddOrderPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;

        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{
        message.payload
    };

    std::uint64_t timestamp{0};

    if (!reader.read_u48_be(timestamp)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    std::uint64_t order_id{0};

    if (!reader.read_u64_be(order_id)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    std::uint8_t side_value{0};

    if (!reader.read_u8(side_value)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    Side side{Side::Buy};

    if (!parse_side(side_value, side)) {
        error_ = ItchDecodeError::InvalidSide;

        return ItchDecodeResult::InvalidMessage;
    }

    std::uint32_t shares{0};

    if (!reader.read_u32_be(shares)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    /*
     * Stock symbol.
     *
     * We don't need it for the simulator yet.
     *
     * Consume the eight bytes but don't copy them.
     */
    std::span<const std::byte> stock;

    if (!reader.read_bytes(8, stock)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    std::uint32_t price{0};

    if (!reader.read_u32_be(price)) {
        error_ = ItchDecodeError::TruncatedPayload;

        return ItchDecodeResult::InvalidMessage;
    }

    /*
     * ITCH prices are represented as fixed-point integers.
     *
     * We currently keep Price as integer ticks, so the
     * decoder passes the raw ITCH price integer through.
     */
    order.timestamp = timestamp;
    order.order_id = OrderId{order_id};
    order.side = side;
    order.quantity = Quantity{shares};
    order.price = Price{
        static_cast<std::int64_t>(price)
    };

    return ItchDecodeResult::Success;
}

} // namespace simulator