#include "simulator/itch/itch_source.hpp"

#include <algorithm>

#include "simulator/itch/byte_reader.hpp"
#include "simulator/itch/itch_decoder.hpp"
#include "simulator/itch/itch_message_types.hpp"

namespace simulator {

namespace {

// ITCH 5.0 message types ItchDecoder does not handle, with their payload sizes.
constexpr std::uint8_t TRADE = 0x50;  // 'P'
constexpr std::size_t TradePayloadSize = 43;
constexpr std::uint8_t TRADING_ACTION = 0x48;  // 'H'
constexpr std::size_t TradingActionPayloadSize = 24;
constexpr std::uint8_t SYSTEM_EVENT = 0x53;  // 'S'
constexpr std::size_t SystemEventPayloadSize = 11;

// The types that become MarketEvents: the order messages, P and H.
bool is_event_message(std::uint8_t type) noexcept
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
    case TRADING_ACTION:
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

// 'H' Stock Trading Action: the symbol's new trading state. Stock, reserved
// byte and reason are left unread.
SourceError decode_trading_action(const ItchMessage& message, MarketEvent& event) noexcept
{
    if (message.payload.size() != TradingActionPayloadSize) {
        return SourceError::InvalidPayloadLength;
    }

    // The size check makes every read succeed.
    ByteReader reader{message.payload};
    std::span<const std::byte> unused;
    std::uint64_t timestamp{0};
    std::uint8_t code{0};

    reader.read_bytes(4, unused);  // locate, tracking number
    reader.read_u48_be(timestamp);
    reader.read_bytes(8, unused);  // stock
    reader.read_u8(code);

    TradingState state{};
    switch (code) {
    case 'T': state = TradingState::Trading; break;
    case 'H': state = TradingState::Halted; break;
    case 'P': state = TradingState::Paused; break;
    case 'Q': state = TradingState::QuotationOnly; break;
    default: return SourceError::InvalidTradingState;
    }

    event = MarketEvent{.timestamp = static_cast<Timestamp>(timestamp), .type = MarketEventType::Status,
                        .trading_state = state};
    return SourceError::None;
}

// An event message as a MarketEvent, timestamp in ITCH ns since midnight and
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
    case TRADING_ACTION:
        return decode_trading_action(message, event);
    default:  // TRADE; next() passes only event messages
        return decode_trade(message, event);
    }

    return result == ItchDecodeResult::Success ? SourceError::None : to_source_error(decoder);
}

} // namespace

ItchSource::ItchSource(
    std::span<const std::byte> data,
    const ItchSourceConfig& config,
    InstrumentRegistry& registry)
    : reader_(data),
      registry_(registry),
      all_tickers_(config.all_tickers),
      require_ticker_(!config.all_tickers && config.tickers.empty()),
      session_midnight_(session_midnight_ns(config.session_date, config.utc_offset)),
      allow_truncated_tail_(config.allow_truncated_tail),
      symbols_(std::size_t{1} << 16, no_symbol),
      has_status_(std::size_t{1} << 16)
{
    if (all_tickers_) {
        return;
    }

    const std::span<const std::string_view> tickers =
        require_ticker_ ? std::span<const std::string_view>{&config.ticker, 1} : config.tickers;

    for (const std::string_view ticker : tickers) {
        std::array<char, 8>& padded = tickers_.emplace_back();  // all NUL, which no R matches

        if (!ticker.empty() && ticker.size() <= padded.size()) {
            padded.fill(' ');
            std::copy(ticker.begin(), ticker.end(), padded.begin());
        }
    }

    if (require_ticker_ && tickers_[0][0] != '\0') {
        std::copy(config.ticker.begin(), config.ticker.end(), instrument_.native_symbol.begin());
    }
}

bool ItchSource::selected(const std::array<char, 8>& stock) const noexcept
{
    return all_tickers_ || std::find(tickers_.begin(), tickers_.end(), stock) != tickers_.end();
}

SourceResult ItchSource::finish(SourceError error) noexcept
{
    done_ = true;
    error_ = error;
    return error == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

SourceResult ItchSource::end_of_data() noexcept
{
    return finish(require_ticker_ && locate_ == 0 ? SourceError::SymbolNotFound : SourceError::None);
}

SourceResult ItchSource::next(MarketEvent& event) noexcept
{
    while (!done_) {
        // Halt sweep after S 'S': one Status Halted per selected locate with no trading state yet.
        while (halt_sweep_ < symbols_.size()) {
            const std::size_t stock_locate = halt_sweep_++;
            if (symbols_[stock_locate] != no_symbol && !has_status_[stock_locate]) {
                has_status_[stock_locate] = true;
                event = MarketEvent{.timestamp = halt_sweep_time_, .symbol = symbols_[stock_locate],
                                    .type = MarketEventType::Status, .trading_state = TradingState::Halted};
                ++statistics_.events;
                return SourceResult::Event;
            }
        }

        message_start_ = reader_.position();

        ItchMessage message;
        const ItchReadResult read = reader_.next(message);

        if (read == ItchReadResult::EndOfBuffer) {
            return end_of_data();
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
            return end_of_data();
        }

        ++statistics_.messages;

        if (message.type == itch::STOCK_DIRECTORY) {
            ItchDecoder decoder;
            ItchStockDirectory directory;

            if (decoder.decode_stock_directory(message, directory) != ItchDecodeResult::Success) {
                return finish(to_source_error(decoder));
            }
            if (!selected(directory.stock)) {
                ++statistics_.filtered;
                continue;
            }

            // native_symbol: the stock field without its space padding.
            Instrument instrument = instrument_;
            const auto& stock = directory.stock;
            const auto end = std::find_if_not(stock.rbegin(), stock.rend(), [](char c) { return c == ' '; });
            instrument.native_symbol = {};
            std::copy(stock.begin(), end.base(), instrument.native_symbol.begin());

            SymbolId symbol{0};
            const RegisterResult added = registry_.add(instrument, symbol);
            if (added == RegisterResult::Conflict || added == RegisterResult::Full) {
                return finish(SourceError::InstrumentRejected);
            }
            symbols_[directory.stock_locate] = symbol;
            locate_ = directory.stock_locate;
            symbol_ = symbol;
            continue;
        }

        if (!is_event_message(message.type)) {
            // ITCH 5.0: a security missing from the pre-open trading action spin
            // is halted from the start of system hours (S, event code 'S').
            if (message.type == SYSTEM_EVENT && message.payload.size() == SystemEventPayloadSize &&
                message.payload[10] == std::byte{'S'}) {
                ByteReader reader{message.payload};
                std::span<const std::byte> unused;
                std::uint64_t ns{0};
                reader.read_bytes(4, unused);  // locate, tracking number
                reader.read_u48_be(ns);
                halt_sweep_ = 0;
                halt_sweep_time_ = static_cast<Timestamp>(ns) + session_midnight_;
            }
            ++statistics_.skipped;
            continue;
        }

        // Unmapped locates are dropped before decoding; a payload too short for one fails in decode.
        SymbolId symbol{0};
        std::uint16_t stock_locate{0};
        if (message.payload.size() >= 2) {
            stock_locate = peek_locate(message.payload);
            symbol = symbols_[stock_locate];
            if (symbol == no_symbol) {
                ++statistics_.filtered;
                continue;
            }
        }

        if (const SourceError error = decode(message, event); error != SourceError::None) {
            return finish(error);
        }
        if (message.type == TRADING_ACTION) {
            has_status_[stock_locate] = true;
        }
        event.timestamp += session_midnight_;
        event.symbol = symbol;
        ++statistics_.events;
        return SourceResult::Event;
    }

    return error_ == SourceError::None ? SourceResult::EndOfStream : SourceResult::Error;
}

} // namespace simulator
