#pragma once

#include <cstdint>

#include "simulator/market/market_event.hpp"

namespace simulator {

enum class SourceResult : std::uint8_t {
    Event,
    EndOfStream,
    Error
};

enum class SourceError : std::uint8_t {
    None,
    TruncatedMessage,      // framing: missing length or short message
    InvalidLength,         // framing: zero length
    InvalidPayloadLength,  // ITCH: size does not match the type
    InvalidSide,
    SymbolNotFound,        // ITCH: end of stream without an R message for the ticker
    InstrumentRejected,    // InstrumentRegistry::add returned Conflict or Full
    InvalidHeader,         // CSV
    InvalidRow,            // CSV: wrong column count
    InvalidNumber,         // CSV: not a number
    NumberOutOfRange,      // CSV: e.g. quantity > UINT64_MAX or negative
    InvalidEventType,      // CSV
    InvalidTradingState,   // ITCH H: unknown trading state byte
    InvalidPrintable       // ITCH C: printable flag neither Y nor N
};

// Pull-based stream of MarketEvents with non-decreasing timestamps. EndOfStream and Error
// are sticky; an event's symbol is registered before the event is emitted.
class MarketEventSource
{
public:
    virtual ~MarketEventSource() = default;

    virtual SourceResult next(MarketEvent& event) noexcept = 0;

    // Where the last event came from or the error occurred: ITCH byte offset,
    // CSV 1-based line, VectorSource 1-based index.
    virtual std::uint64_t position() const noexcept = 0;

    // Valid after next() returned Error.
    virtual SourceError error() const noexcept = 0;
};

} // namespace simulator
