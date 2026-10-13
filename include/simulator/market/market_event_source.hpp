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
    InvalidRow,            // text (CSV, LOBSTER): wrong column count
    InvalidNumber,         // text: not a number
    NumberOutOfRange,      // text: e.g. quantity > UINT64_MAX or negative
    InvalidEventType,      // text
    InvalidTradingState    // a status names an unknown trading state (ITCH H's state byte, LOBSTER 7's price)
};

// A pull-based stream of MarketEvents. Contract, checked for every source by
// SourceConformanceTest (tests/support/source_conformance.hpp):
//   - next() returns events with non-decreasing timestamps (epoch ns, UTC).
//   - After EndOfStream or Error, further calls return the same result.
//   - Each source applies its own symbol filter.
//   - An event's symbol is registered in the InstrumentRegistry before the
//     event is emitted.
//   - order_id is unique within one symbol while the order is live.
class MarketEventSource
{
public:
    virtual ~MarketEventSource() = default;

    virtual SourceResult next(MarketEvent& event) noexcept = 0;

    // ITCH: byte offset of the start of the last message read or failed.
    // CSV, LOBSTER: 1-based line. Vector: 1-based index.
    virtual std::uint64_t position() const noexcept = 0;

    // Valid after next() returned Error.
    virtual SourceError error() const noexcept = 0;
};

} // namespace simulator
