#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "simulator/itch/itch_message_reader.hpp"
#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_event_source.hpp"

namespace simulator {

// Which tickers: all_tickers, else tickers if not empty, else ticker.
struct ItchSourceConfig
{
    // 1-8 ASCII chars matched against R's stock field; without its R the stream
    // ends in SymbolNotFound.
    std::string_view ticker{};

    // Matched like `ticker`; one with no R is never registered, which is not an error.
    std::span<const std::string_view> tickers{};

    bool all_tickers{false};  // every R
    std::chrono::year_month_day session_date{};
    std::chrono::minutes utc_offset{0};  // e.g. -5h for New York in winter

    // A partial last message ends the stream (EndOfStream, truncated_tail()) instead of failing.
    bool allow_truncated_tail{false};
};

struct ItchSourceStatistics
{
    std::uint64_t messages{0};  // every framed message
    std::uint64_t filtered{0};  // messages for unselected symbols (event messages and R)
    std::uint64_t skipped{0};   // types that become no event (S, Q, I, h, ...)
    std::uint64_t events{0};    // MarketEvents emitted, the halt sweep's included
};

// MarketEvents of the selected tickers from NASDAQ TotalView-ITCH 5.0 bytes; each selected R
// registers an instrument. The caller keeps `data` and `registry` alive.
class ItchSource final : public MarketEventSource
{
private:
    static constexpr SymbolId no_symbol = ~SymbolId{0};

    ItchMessageReader reader_;
    InstrumentRegistry& registry_;
    std::vector<std::array<char, 8>> tickers_;  // space-padded; all NUL if invalid
    bool all_tickers_{false};
    bool require_ticker_{false};  // config.ticker: SymbolNotFound without its R
    Instrument instrument_{.venue = {"XNAS"}, .price_scale = 4, .quantity_scale = 0, .tick_size = 0};
    Timestamp session_midnight_{0};
    bool allow_truncated_tail_{false};

    std::vector<SymbolId> symbols_;  // index = locate; no_symbol until its selected R
    std::vector<bool> has_status_;   // index = locate: an H or the halt sweep gave it a trading state
    // ITCH halts every locate with no H by the start-of-system-hours S; this sweep emits those halts.
    std::size_t halt_sweep_{std::size_t{1} << 16};  // next locate the halt sweep checks; 65,536 = none
    Timestamp halt_sweep_time_{0};                  // of the S that started the sweep, epoch ns
    std::uint16_t locate_{0};        // of the last selected R
    SymbolId symbol_{0};
    std::uint64_t message_start_{0};
    bool done_{false};
    bool truncated_tail_{false};
    SourceError error_{SourceError::None};
    std::uint8_t failed_message_type_{0};
    ItchSourceStatistics statistics_{};

    SourceResult finish(SourceError error, std::uint8_t message_type = 0) noexcept;
    SourceResult end_of_data() noexcept;  // SymbolNotFound or EndOfStream
    bool selected(const std::array<char, 8>& stock) const noexcept;

public:
    // Allocates the 65,536-entry locate tables.
    ItchSource(
        std::span<const std::byte> data,
        const ItchSourceConfig& config,
        InstrumentRegistry& registry);

    // Error/SymbolNotFound if the stream ends before the R of config.ticker.
    SourceResult next(MarketEvent& event) noexcept override;

    // Byte offset of the last message read or failed; the buffer size at its end.
    std::uint64_t position() const noexcept override
    {
        return message_start_;
    }

    SourceError error() const noexcept override
    {
        return error_;
    }

    // Type byte of the message at position() that failed to decode or register; 0 otherwise.
    std::uint8_t failed_message_type() const noexcept
    {
        return failed_message_type_;
    }

    // True once a partial last message ended the stream (allow_truncated_tail).
    bool truncated_tail() const noexcept
    {
        return truncated_tail_;
    }

    // Locate of the last selected R; 0 before it.
    std::uint16_t locate() const noexcept
    {
        return locate_;
    }

    // Valid once locate() != 0; of the same R as locate().
    SymbolId symbol() const noexcept
    {
        return symbol_;
    }

    const ItchSourceStatistics& statistics() const noexcept
    {
        return statistics_;
    }

    // What a selected R registers, set at construction; native_symbol is empty
    // with tickers or all_tickers.
    const Instrument& instrument() const noexcept
    {
        return instrument_;
    }
};

} // namespace simulator
