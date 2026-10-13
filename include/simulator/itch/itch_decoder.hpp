#pragma once

#include <cstdint>

#include "simulator/itch/byte_reader.hpp"
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
    TruncatedPayload,
    InvalidPrintable
};

class ItchDecoder
{
private:
    ItchDecodeError error_{ItchDecodeError::None};

    // Common body prefix: locate (2), tracking number (2, discarded), timestamp (6, ns since midnight).
    bool read_header(ByteReader& reader,std::uint16_t& stock_locate,Timestamp& timestamp) noexcept;

    // Reads the Add Order fields shared by 'A' and 'F'.
    ItchDecodeResult read_add_order(ByteReader& reader,ItchAddOrder& order) noexcept;

public:
    ItchDecodeResult decode_add_order(const ItchMessage& message,ItchAddOrder& order) noexcept;
    ItchDecodeResult decode_add_order_mpid(const ItchMessage& message,ItchAddOrder& order) noexcept;
    ItchDecodeResult decode_order_executed(const ItchMessage& message,ItchOrderExecuted& order) noexcept;
    ItchDecodeResult decode_order_executed_with_price(const ItchMessage& message,ItchOrderExecutedWithPrice& order) noexcept;
    ItchDecodeResult decode_order_cancel(const ItchMessage& message,ItchOrderCancel& order) noexcept;
    ItchDecodeResult decode_order_delete(const ItchMessage& message,ItchOrderDelete& order) noexcept;
    ItchDecodeResult decode_order_replace(const ItchMessage& message,ItchOrderReplace& order) noexcept;
    ItchDecodeResult decode_stock_directory(const ItchMessage& message,ItchStockDirectory& directory) noexcept;
    constexpr ItchDecodeError error() const noexcept{
        return error_;
    }
};

} // namespace simulator