#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "simulator/market/market_event_source.hpp"

namespace simulator {

// MarketEvents of one registered instrument from CSV bytes the caller keeps alive.
// Header: timestamp,event_type,order_id,side,price,quantity[,new_order_id]; no quoting.
// Columns used: ADD id,side,price,qty; CANCEL id,qty; DELETE id; EXECUTE id,qty[,price];
// REPLACE id,price,qty,new_id; TRADE side,price,qty[,id]. Empty optional values are 0.
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
        : text_(reinterpret_cast<const char*>(data.data()), data.size()), symbol_(symbol)
    {}

    SourceResult next(MarketEvent& event) noexcept override;

    // Counts the header and blank lines; 0 before the first line.
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
