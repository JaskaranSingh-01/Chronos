#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "simulator/market/market_event_source.hpp"

namespace simulator {

// The MarketEvents of one instrument from a CSV byte span, in file order,
// each stamped with the SymbolId the caller registered.
//
//   timestamp,event_type,order_id,side,price,quantity[,new_order_id]
//
// The first non-empty line is exactly the 6-column header or the same plus
// ",new_order_id"; every row then has that many columns. Lines end in LF or
// CRLF and blank lines are skipped. No quoting, trimming or BOM.
//
// Values are ASCII digits: timestamp in epoch ns (UTC), price in ticks at the
// instrument's price_scale, quantity at its quantity_scale. An empty optional
// column means 0. Columns an action ignores are still validated if present;
// the event gets 0 (or Buy) there.
//
//   Action    required                                optional
//   ADD       order_id, side, price, quantity
//   CANCEL    order_id, quantity
//   DELETE    order_id
//   EXECUTE   order_id, quantity                      price (0 = unknown)
//   REPLACE   order_id, price, quantity, new_order_id (side is inherited)
//   TRADE     side, price, quantity                   order_id (0 = none)
//
// Errors are sticky: InvalidHeader, InvalidRow (column count),
// InvalidEventType, InvalidSide (BUY or SELL), InvalidNumber (empty when
// required, or not all digits) and NumberOutOfRange (a leading '-', or above
// INT64_MAX for timestamp and price, UINT64_MAX otherwise).
// The caller registers the instrument, passes its SymbolId and keeps `data`
// alive (files are opened with MappedFile).
class CsvSource final : public MarketEventSource
{
private:
    std::string_view text_;
    std::size_t offset_{0};    // start of the next line
    std::uint64_t line_{0};    // 1-based, the last line read
    std::size_t columns_{0};   // 0 until the header, then 6 or 7
    SymbolId symbol_;
    bool done_{false};
    SourceError error_{SourceError::None};

    SourceResult finish(SourceError error) noexcept;

public:
    CsvSource(std::span<const std::byte> data, SymbolId symbol) noexcept
        : text_(reinterpret_cast<const char*>(data.data()), data.size()), symbol_(symbol) {}

    SourceResult next(MarketEvent& event) noexcept override;

    // 1-based number of the last line read, counting the header and blank
    // lines; 0 before the first.
    std::uint64_t position() const noexcept override
    {
        return line_;
    }

    SourceError error() const noexcept override
    {
        return error_;
    }
};

} // namespace simulator
