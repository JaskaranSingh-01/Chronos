#include "simulator/itch/itch_decoder.hpp"

#include <cstddef>
#include <cstdint>

#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_message_types.hpp"

namespace simulator {

namespace {

/*
 * ITCH 5.0 payload sizes, excluding the type byte. Every payload
 * starts with stock locate (2), tracking number (2), timestamp (6).
 */
constexpr std::size_t AddOrderPayloadSize = 35;
constexpr std::size_t AddOrderMpidPayloadSize = 39;
constexpr std::size_t OrderExecutedPayloadSize = 30;
constexpr std::size_t OrderExecutedWithPricePayloadSize = 35;
constexpr std::size_t OrderCancelPayloadSize = 22;
constexpr std::size_t OrderDeletePayloadSize = 18;
constexpr std::size_t OrderReplacePayloadSize = 34;
constexpr std::size_t StockDirectoryPayloadSize = 38;

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

bool parse_printable(
    std::uint8_t value,
    bool& printable
) noexcept
{
    if (value == 'Y' || value == 'N') {
        printable = value == 'Y';
        return true;
    }

    return false;
}

} // namespace

bool ItchDecoder::read_header(
    ByteReader& reader,
    std::uint16_t& stock_locate,
    Timestamp& timestamp
) noexcept
{
    std::uint16_t tracking_number{0};
    std::uint64_t raw_timestamp{0};

    if (!reader.read_u16_be(stock_locate) ||
        !reader.read_u16_be(tracking_number) ||
        !reader.read_u48_be(raw_timestamp)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return false;
    }

    timestamp = static_cast<Timestamp>(raw_timestamp);

    return true;
}

ItchDecodeResult ItchDecoder::decode_add_order(
    const ItchMessage& message,
    ItchAddOrder& order
) noexcept
{
    error_ = ItchDecodeError::None;
    order = ItchAddOrder{};

    if (message.type != itch::ADD_ORDER) {
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

    return read_add_order(reader, order);
}

ItchDecodeResult ItchDecoder::decode_add_order_mpid(
    const ItchMessage& message,
    ItchAddOrder& order
) noexcept
{
    error_ = ItchDecodeError::None;
    order = ItchAddOrder{};

    if (message.type != itch::ADD_ORDER_MPID) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != AddOrderMpidPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{
        message.payload
    };

    /*
     * 'F' is 'A' plus a trailing 4-byte MPID attribution. The
     * length check above accounts for it; Chronos doesn't keep it.
     */
    return read_add_order(reader, order);
}

ItchDecodeResult ItchDecoder::read_add_order(
    ByteReader& reader,
    ItchAddOrder& order
) noexcept
{
    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
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
    order.stock_locate = stock_locate;
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

    if (message.type != itch::ORDER_EXECUTED) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderExecutedPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
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

    order.timestamp = timestamp;
    order.stock_locate = stock_locate;
    order.order_id = OrderId{order_id};
    order.quantity = Quantity{executed_shares};
    order.match_number = match_number;

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_executed_with_price(const ItchMessage& message,ItchOrderExecutedWithPrice& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderExecutedWithPrice{};

    if (message.type != itch::ORDER_EXECUTED_WITH_PRICE) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderExecutedWithPricePayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
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

    std::uint8_t printable_value{0};

    if (!reader.read_u8(printable_value)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    bool printable{false};

    if (!parse_printable(printable_value, printable)) {
        error_ = ItchDecodeError::InvalidPrintable;
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint32_t execution_price{0};

    if (!reader.read_u32_be(execution_price)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    order.timestamp = timestamp;
    order.stock_locate = stock_locate;
    order.order_id = OrderId{order_id};
    order.quantity = Quantity{executed_shares};
    order.match_number = match_number;
    order.printable = printable;
    order.price = Price{
        static_cast<std::int64_t>(execution_price)
    };

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_cancel(const ItchMessage& message,ItchOrderCancel& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderCancel{};

    if (message.type != itch::ORDER_CANCEL) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderCancelPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
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

    order.timestamp = timestamp;
    order.stock_locate = stock_locate;
    order.order_id = OrderId{order_id};
    order.quantity = Quantity{cancelled_shares};

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_delete(const ItchMessage& message,ItchOrderDelete& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderDelete{};

    if (message.type != itch::ORDER_DELETE) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderDeletePayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint64_t order_id{0};

    if (!reader.read_u64_be(order_id)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    order.timestamp = timestamp;
    order.stock_locate = stock_locate;
    order.order_id = OrderId{order_id};

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_order_replace(const ItchMessage& message,ItchOrderReplace& order) noexcept{
    error_ = ItchDecodeError::None;
    order = ItchOrderReplace{};

    if (message.type != itch::REPLACE_ORDER) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != OrderReplacePayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint64_t original_order_id{0};

    if (!reader.read_u64_be(original_order_id)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint64_t replacement_order_id{0};

    if (!reader.read_u64_be(replacement_order_id)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint32_t shares{0};

    if (!reader.read_u32_be(shares)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    std::uint32_t price{0};

    if (!reader.read_u32_be(price)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    order.timestamp = timestamp;
    order.stock_locate = stock_locate;

    order.original_order_id = OrderId{original_order_id};

    order.replacement_order_id = OrderId{replacement_order_id};

    order.quantity = Quantity{shares};

    order.price = Price{
        static_cast<std::int64_t>(price)
    };

    return ItchDecodeResult::Success;
}

ItchDecodeResult ItchDecoder::decode_stock_directory(const ItchMessage& message,ItchStockDirectory& directory) noexcept{
    error_ = ItchDecodeError::None;
    directory = ItchStockDirectory{};

    if (message.type != itch::STOCK_DIRECTORY) {
        error_ = ItchDecodeError::InvalidMessageType;
        return ItchDecodeResult::UnsupportedMessage;
    }

    if (message.payload.size() != StockDirectoryPayloadSize) {
        error_ = ItchDecodeError::InvalidPayloadLength;
        return ItchDecodeResult::InvalidMessage;
    }

    ByteReader reader{message.payload};

    std::uint16_t stock_locate{0};
    Timestamp timestamp{0};

    if (!read_header(reader, stock_locate, timestamp)) {
        return ItchDecodeResult::InvalidMessage;
    }

    std::span<const std::byte> stock;

    if (!reader.read_bytes(8, stock)) {
        error_ = ItchDecodeError::TruncatedPayload;
        return ItchDecodeResult::InvalidMessage;
    }

    /*
     * The remaining 20 bytes (market category, round lot size,
     * flags, ...) are not used by Chronos and are left unread.
     */
    directory.timestamp = timestamp;
    directory.stock_locate = stock_locate;

    for (std::size_t i = 0; i < directory.stock.size(); ++i) {
        directory.stock[i] = static_cast<char>(stock[i]);
    }

    return ItchDecodeResult::Success;
}

} // namespace simulator
