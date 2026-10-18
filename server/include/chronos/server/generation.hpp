#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "chronos/server/command.hpp"
#include "chronos/server/session_strategy.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/market/instrument_registry.hpp"

namespace simulator::server {

// An ITCH ticker, NUL-padded: a generation's input. All NUL before the session's first symbol switch.
using Ticker = std::array<char, 8>;

// The ticker without its padding.
inline std::string_view name(const Ticker& ticker) noexcept
{
    return {ticker.data(), static_cast<std::size_t>(std::find(ticker.begin(), ticker.end(), '\0') - ticker.begin())};
}

// `text` as a Ticker; false if it is longer than 8 characters or holds a NUL.
inline bool to_ticker(std::string_view text, Ticker& ticker) noexcept
{
    if (text.size() > ticker.size() || text.find('\0') != std::string_view::npos) {
        return false;
    }
    ticker.fill('\0');
    std::copy(text.begin(), text.end(), ticker.begin());
    return true;
}

// Builds a fresh market stream of `ticker` for each generation of a session.
using SourceFactory = std::function<std::unique_ptr<MarketEventSource>(std::string_view ticker)>;

// What every generation of a session shares.
struct SessionConfig
{
    std::string session_id{};
    std::string input{};  // identifies the market data in the command log's header
    Instrument instrument{};
    SymbolId symbol{0};
    std::size_t book_capacity{std::size_t{1} << 20};
    LatencyConfig latency{};
    RiskLimits limits{};
    FeeSchedule fees{};
    ReplayOptions options{};
    std::uint64_t source_size{0};  // the source's position() at the end of its stream; 0 if unknown
};

// One generation of a session: a fresh source, book, simulator, portfolio and strategy under one EventLoop.
struct Generation
{
    std::unique_ptr<MarketEventSource> source;
    MarketBook book;
    ExecutionSimulator execution;
    Portfolio portfolio;
    SessionStrategy strategy;
    EventLoop<SessionStrategy> loop;

    Generation(const SessionConfig& config, std::unique_ptr<MarketEventSource> market, const StrategySpec& spec);
};

struct Applied
{
    ErrorCode error{ErrorCode::None};
    OrderId order_id{0};  // the new order's id, or the cancelled one's
};

// Applies one command at the loop's clock through the gateway strategies use.
Applied apply_command(Generation& generation, const Command& command) noexcept;

// Fingerprints of a generation's state, for the command log's trailer.
struct Digests
{
    std::uint64_t book{0};       // MarketBook::digest()
    std::uint64_t execution{0};  // the execution counters and our resting orders
    std::uint64_t portfolio{0};  // position, cash, PnL, fees, drawdown and open orders

    bool operator==(const Digests&) const = default;
};

Digests digests(const Generation& generation) noexcept;

}  // namespace simulator::server
