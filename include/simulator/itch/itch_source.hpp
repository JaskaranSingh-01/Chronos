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
    // One ticker: 1-8 ASCII chars, matched against R's space-padded stock
    // field. Anything else never matches, so the stream ends in SymbolNotFound.
    std::string_view ticker;

    // Several tickers, matched as `ticker` is. One whose R never comes is not
    // an error: it is never registered, which is how the caller finds it missing.
    std::span<const std::string_view> tickers;

    bool all_tickers{false};  // every R
    std::chrono::year_month_day session_date{};
    std::chrono::minutes utc_offset{0};  // e.g. -5h for New York in winter

    // A file cut at a byte count ends mid-message. When true, that partial
    // message ends the stream (EndOfStream, truncated_tail()) instead of
    // Error/TruncatedMessage.
    bool allow_truncated_tail{false};
};

struct ItchSourceStatistics
{
    std::uint64_t messages{0};  // every framed message
    std::uint64_t filtered{0};  // messages for unselected symbols (order messages and R)
    std::uint64_t skipped{0};   // types Chronos does not use (S, H, Q, I, ...)
    std::uint64_t events{0};    // MarketEvents emitted
};

// The MarketEvents of the selected tickers from a NASDAQ TotalView-ITCH 5.0
// byte span, in file order. A selected ticker's R registers {XNAS, ticker,
// price_scale 4, quantity_scale 0, tick_size 0} and maps its locate to the
// SymbolId; order messages (A F E C X D U P) for an unmapped locate are
// skipped before decoding, so they are never validated.
// The caller keeps `data` and `registry` alive.
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
    std::uint16_t locate_{0};        // of the last selected R
    SymbolId symbol_{0};
    std::uint64_t message_start_{0};
    bool done_{false};
    bool truncated_tail_{false};
    SourceError error_{SourceError::None};
    ItchSourceStatistics statistics_{};

    SourceResult finish(SourceError error) noexcept;
    SourceResult end_of_data() noexcept;  // SymbolNotFound or EndOfStream
    bool selected(const std::array<char, 8>& stock) const noexcept;

public:
    // Allocates the 65,536-entry locate table.
    ItchSource(
        std::span<const std::byte> data,
        const ItchSourceConfig& config,
        InstrumentRegistry& registry);

    // Error/SymbolNotFound if the stream ends before the R of config.ticker.
    SourceResult next(MarketEvent& event) noexcept override;

    // Byte offset of the start of the last message read or failed; at the
    // end of a whole buffer, its size.
    std::uint64_t position() const noexcept override
    {
        return message_start_;
    }

    SourceError error() const noexcept override
    {
        return error_;
    }

    // True once a partial last message ended the stream (allow_truncated_tail).
    bool truncated_tail() const noexcept
    {
        return truncated_tail_;
    }

    // 0 until the ticker's R is seen. With tickers or all_tickers, of the
    // last selected R.
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

    // What the ticker's R registers; complete from construction, before the R.
    // With tickers or all_tickers, native_symbol is empty: each R names its own.
    const Instrument& instrument() const noexcept
    {
        return instrument_;
    }
};

} // namespace simulator
