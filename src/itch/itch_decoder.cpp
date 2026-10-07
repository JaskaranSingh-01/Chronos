#include "simulator/itch/itch_decoder.hpp"

#include <cstddef>
#include <cstdint>

#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_message_types.hpp"

namespace simulator {

namespace {

constexpr std::uint8_t AddOrderMessageType = 0x41; // 'A'
constexpr std::uint8_t OrderExecutedMessageType = 0x45; // 'E'
constexpr std::uint8_t OrderCancelMessageType = 0x58;   // 'X'
constexpr std::uint8_t OrderDeleteMessageType = 0x44;   // 'D'

constexpr std::size_t OrderExecutedPayloadSize = 26;
constexpr std::size_t OrderCancelPayloadSize = 18;
constexpr std::size_t OrderDeletePayloadSize = 14;

constexpr std::size_t AddOrderPayloadSize = 31;

static constexpr std::uint8_t ADD_ORDER = 0x41;
static constexpr std::uint8_t ORDER_EXECUTED = 0x45;
static constexpr std::uint8_t ORDER_CANCEL = 0x58;
static constexpr std::uint8_t ORDER_DELETE = 0x44;

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

ItchDecodeResult ItchDecoder::decode_order_executed(const ItchMessage& message,ItchOrderExecuted& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderExecuted{};

    if (message.type != OrderExecutedMessageType) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderExecutedPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

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

    std::uint32_t executed_shares{0};

    if (!reader.read_u32_be(executed_shares)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint64_t match_number{0};

    if (!reader.read_u64_be(match_number)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    order.timestamp = static_cast<Timestamp>(timestamp);
    order.order_id = OrderId{order_id};
    order.quantity = Quantity{executed_shares};
    order.match_number = match_number;

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_cancel(const ItchMessage& message,ItchOrderCancel& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderCancel{};

    if (message.type != OrderCancelMessageType) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderCancelPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

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

    std::uint32_t cancelled_shares{0};

    if (!reader.read_u32_be(cancelled_shares)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    order.timestamp = static_cast<Timestamp>(timestamp);
    order.order_id = OrderId{order_id};
    order.quantity = Quantity{cancelled_shares};

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_delete(const ItchMessage& message,ItchOrderDelete& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderDelete{};

    if (message.type != OrderDeleteMessageType) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderDeletePayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

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

    order.timestamp = static_cast<Timestamp>(timestamp);
    order.order_id = OrderId{order_id};

    return ItchDecodeResult::Success;
}

} // namespace simulator