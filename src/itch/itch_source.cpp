#include "simulator/itch/itch_source.hpp"

#include <algorithm>

#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_types.hpp"

namespace simulator {

namespace {

// ponytail: 'P' is decoded here rather than in ItchDecoder, its only user.
// Move it (and the constant to itch_message_types.hpp) when a second one appears.
constexpr std::uint8_t TRADE = 0x50;  // 'P'
constexpr std::size_t TradePayloadSize = 43;

bool is_order_message(std::uint8_t type) noexcept
{
    switch (type) {
    case itch::ADD_ORDER:
    case itch::ADD_ORDER_MPID:
    case itch::ORDER_EXECUTED:
    case itch::ORDER_EXECUTED_WITH_PRICE:
    case itch::ORDER_CANCEL:
    case itch::ORDER_DELETE:
    case itch::REPLACE_ORDER:
    case TRADE:
        return true;
    default:
        return false;
    }
}

// Payload bytes 0-1: the stock locate every ITCH 5.0 body starts with.
std::uint16_t peek_locate(std::span<const std::byte> payload) noexcept
{
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(payload[0]) << 8 | std::to_integer<std::uint16_t>(payload[1]));
}

// The decoders check the payload size first, so TruncatedPayload cannot
// happen; a bad C printable flag has no SourceError of its own.
SourceError to_source_error(const ItchDecoder& decoder) noexcept
{
    return decoder.error() == ItchDecodeError::InvalidSide
        ? SourceError::InvalidSide
        : SourceError::InvalidPayloadLength;
}

SourceError decode_trade(const ItchMessage& message, MarketEvent& event) noexcept
{
    if (message.payload.size() != TradePayloadSize) {
        return SourceError::InvalidPayloadLength;
    }

    // The size check makes every read succeed. Match number (8) is left unread.
    ByteReader reader{message.payload};
    std::span<const std::byte> unused;
    std::uint64_t timestamp{0};
    std::uint64_t order_id{0};
    std::uint8_t side{0};
    std::uint32_t shares{0};
    std::uint32_t price{0};

    reader.read_bytes(4, unused);  // locate, tracking number
    reader.read_u48_be(timestamp);
    reader.read_u64_be(order_id);
    reader.read_u8(side);
    reader.read_u32_be(shares);
    reader.read_bytes(8, unused);  // stock
    reader.read_u32_be(price);

    if (side != 'B' && side != 'S') {
        return SourceError::InvalidSide;
    }

    event = MarketEvent{
        .timestamp = static_cast<Timestamp>(timestamp),
        .order_id = order_id,
        .price = Price{price},
        .quantity = Quantity{shares},
        .type = MarketEventType::Trade,
        .side = side == 'B' ? Side::Buy : Side::Sell};
    return SourceError::None;
}

// An order message as a MarketEvent, timestamp in ITCH ns since midnight and
// symbol unset. None on success.
SourceError decode(const ItchMessage& message, MarketEvent& event) noexcept
{
    using T = MarketEventType;

    ItchDecoder decoder;
    ItchDecodeResult result{};

    // On a failed decode `event` holds zeros, and the caller never emits it.
    switch (message.type) {
    case itch::ADD_ORDER:
    case itch::ADD_ORDER_MPID: {
        ItchAddOrder m;
        result = message.type == itch::ADD_ORDER
            ? decoder.decode_add_order(message, m)
            : decoder.decode_add_order_mpid(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.order_id, .price = m.price,
                 .quantity = m.quantity, .type = T::Add, .side = m.side};
        break;
    }
    case itch::ORDER_EXECUTED: {
        ItchOrderExecuted m;
        result = decoder.decode_order_executed(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.order_id, .quantity = m.quantity,
                 .type = T::Execute};
        break;
    }
    case itch::ORDER_EXECUTED_WITH_PRICE: {
        ItchOrderExecutedWithPrice m;
        result = decoder.decode_order_executed_with_price(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.order_id, .price = m.price,
                 .quantity = m.quantity, .type = T::Execute};
        break;
    }
    case itch::ORDER_CANCEL: {
        ItchOrderCancel m;
        result = decoder.decode_order_cancel(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.order_id, .quantity = m.quantity,
                 .type = T::Cancel};
        break;
    }
    case itch::ORDER_DELETE: {
        ItchOrderDelete m;
        result = decoder.decode_order_delete(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.order_id, .type = T::Delete};
        break;
    }
    case itch::REPLACE_ORDER: {
        ItchOrderReplace m;
        result = decoder.decode_order_replace(message, m);
        event = {.timestamp = m.timestamp, .order_id = m.original_order_id,
                 .new_order_id = m.replacement_order_id, .price = m.price,
                 .quantity = m.quantity, .type = T::Replace};
        break;
    }
    default:  // TRADE; next() only passes order messages
        return decode_trade(message, event);
    }

    return result == ItchDecodeResult::Success ? SourceError::None : to_source_error(decoder);
}

} // namespace

ItchSource::ItchSource(
    std::span<const std::byte> data,
    const ItchSourceConfig& config,
    InstrumentRegistry& registry) noexcept
    : reader_(data),
      registry_(registry),
      session_midnight_(session_midnight_ns(config.session_date, config.utc_offset)),
      allow_truncated_tail_(config.allow_truncated_tail)
{
    const std::string_view ticker = config.ticker;

    if (ticker.empty() || ticker.size() > ticker_.size()) {
        return;  // ticker_ stays all NUL, which no R matches
    }
    ticker_.fill(' ');
    std::copy(ticker.begin(), ticker.end(), ticker_.begin());
    std::copy(ticker.begin(), ticker.end(), instrument_.native_symbol.begin());
}

SourceResult ItchSource::finish(SourceError error) noexcept
{
    done_ = true;
    error_ = error;
    return error == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

SourceResult ItchSource::next(MarketEvent& event) noexcept
{
    while (!done_) {
        message_start_ = reader_.position();

        ItchMessage message;
        const ItchReadResult read = reader_.next(message);

        if (read == ItchReadResult::EndOfBuffer) {
            return finish(locate_ == 0 ? SourceError::SymbolNotFound : SourceError::None);
        }
        if (read == ItchReadResult::Error) {
            if (reader_.error() == ItchReadError::InvalidLength) {
                return finish(SourceError::InvalidLength);
            }
            // MissingLength or TruncatedMessage: the buffer ends inside a message.
            if (!allow_truncated_tail_) {
                return finish(SourceError::TruncatedMessage);
            }
            truncated_tail_ = true;
            return finish(locate_ == 0 ? SourceError::SymbolNotFound : SourceError::None);
        }

        ++statistics_.messages;

        if (message.type == itch::STOCK_DIRECTORY) {
            ItchDecoder decoder;
            ItchStockDirectory directory;

            if (decoder.decode_stock_directory(message, directory) != ItchDecodeResult::Success) {
                return finish(to_source_error(decoder));
            }
            if (directory.stock != ticker_) {
                ++statistics_.filtered;
                continue;
            }

            const RegisterResult added = registry_.add(instrument_, symbol_);
            if (added == RegisterResult::Conflict || added == RegisterResult::Full) {
                return finish(SourceError::InstrumentRejected);
            }
            locate_ = directory.stock_locate;
            continue;
        }

        if (!is_order_message(message.type)) {
            ++statistics_.skipped;
            continue;
        }

        // Before decoding: other locates are dropped unvalidated. A payload
        // too short to hold a locate goes on to decode, which rejects it.
        if (message.payload.size() >= 2 &&
            (locate_ == 0 || peek_locate(message.payload) != locate_)) {
            ++statistics_.filtered;
            continue;
        }

        if (const SourceError error = decode(message, event); error != SourceError::None) {
            return finish(error);
        }
        event.timestamp += session_midnight_;
        event.symbol = symbol_;
        ++statistics_.events;
        return SourceResult::Event;
    }

    return error_ == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

} // namespace simulator
