#pragma once

#include <algorithm>
#include <array>
#include <chrono>
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

// ponytail: 32 days per generation or run (~8 min of backtest); raise it, minding the report ring's record size.
inline constexpr std::size_t max_days = 32;

// A generation's or a run's days, oldest first.
struct DayList
{
    std::array<std::chrono::year_month_day, max_days> day{};
    std::uint8_t count{0};

    bool operator==(const DayList&) const = default;
};

// What one generation, or one Lab run, replays and how. Never changed once it starts.
struct RunSpec
{
    Ticker ticker{};
    DayList days{};
    LatencyConfig latency{};
    FeeSchedule fees{};
    RiskLimits limits{};
    TradingWindow window{TradingWindow::FullDay};
    StrategySpec strategy{};  // attached from the first event
};

// Builds a fresh market stream of `ticker` over `days` for each generation of a session.
using SourceFactory = std::function<std::unique_ptr<MarketEventSource>(const Ticker& ticker, const DayList& days)>;

// What every generation of a session shares.
struct SessionConfig
{
    std::string session_id{};
    Instrument instrument{};
    SymbolId symbol{0};
    std::size_t book_capacity{std::size_t{1} << 20};
    std::uint64_t check_invariants_every{0};
    bool allow_truncated_tail{false};  // a day cut mid-message ends there instead of failing
    RunSpec defaults{};                // the first generation's: the terminal's latencies, fees and limits
};

// One generation of a session: a fresh source, book, simulator, portfolio and strategy under one EventLoop. Its first
// day's midnight seeds the loop and the simulator, and its days' feed sessions the strategy's window.
struct Generation
{
    RunSpec spec;
    std::unique_ptr<MarketEventSource> source;
    MarketBook book;
    ExecutionSimulator execution;
    Portfolio portfolio;
    SessionStrategy strategy;
    EventLoop<SessionStrategy> loop;

    Generation(const SessionConfig& config, const RunSpec& run, std::unique_ptr<MarketEventSource> market);
};

struct Applied
{
    ErrorCode error{ErrorCode::None};
    OrderId order_id{0};  // the new order's id, or the cancelled one's
};

// Applies one command at the loop's clock through the gateway strategies use.
Applied apply_command(Generation& generation, const Command& command) noexcept;

// Ends the stream: under a Regular window the clock first runs to the last day's close, so its closing cancels run
// even when the input ends before it; then EventLoop::finish().
void finish(Generation& generation) noexcept;

// The one stepping function. A pending Clear starts a new day at its timestamp: the simulator events due before it run
// under the old day, then the session midnight moves to it, then it applies; those due at it run after it.
template <Strategy S>
StepResult step(EventLoop<S>& loop) noexcept
{
    if (const MarketEvent* next = loop.pending(); next != nullptr && next->type == MarketEventType::Clear) {
        const Timestamp boundary = next->timestamp;
        loop.advance_to(boundary - 1);
        loop.set_session_midnight(boundary);
    }
    return loop.step();
}

// Fingerprints of a generation's state, for the command log's trailer.
struct Digests
{
    std::uint64_t book{0};       // MarketBook::digest()
    std::uint64_t execution{0};  // the execution counters and our resting orders
    std::uint64_t portfolio{0};  // position, cash, PnL, fees, drawdown and open orders

    bool operator==(const Digests&) const = default;
};

Digests digests(const Generation& generation) noexcept;

// A Watch's or a Verify's verdict on a re-run of a Lab run, decided in this order (spec §5.5).
enum class Outcome : std::uint8_t {
    InputChanged,   // a day's full re-hash differs from the run's
    DifferentCode,  // this build's source digest differs from the run's
    Reproduced,     // the digests are the run's
    Mismatch
};

}  // namespace simulator::server
