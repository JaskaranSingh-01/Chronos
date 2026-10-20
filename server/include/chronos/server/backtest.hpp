#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "chronos/server/generation.hpp"
#include "chronos/server/itch_input.hpp"

namespace simulator::server {

// The most fills a run lists; its counters count every fill.
// ponytail: 50k rows (~2.8 MB) per run kept in memory; load results on demand past a few hundred runs.
inline constexpr std::size_t max_fill_rows = 50'000;

// `simulator backtest`'s risk limits for `strategy`: its own max position and order size, and one IOC or two quotes
// open at once.
RiskLimits default_limits(const StrategySpec& strategy);

// `simulator backtest`'s settings for `kind`, which a Lab run's omitted keys take: its latencies, fees, params and
// limits, under a Regular window.
RunSpec default_spec(SessionStrategyKind kind);

// One fill of a run.
struct FillRow
{
    Timestamp exchange_ts{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{0};
    Liquidity liquidity{Liquidity::None};
    FillCause rule{FillCause::None};
    std::uint64_t ahead_at_entry{0};
    std::int64_t position{0};  // after the fill

    bool operator==(const FillRow&) const = default;
};

// The PnL and position at the end of an exchange second that had a market event.
struct Sample
{
    std::int64_t second{0};  // epoch seconds
    std::int64_t pnl{0};
    std::int64_t position{0};

    bool operator==(const Sample&) const = default;
};

struct DayResult
{
    std::chrono::year_month_day date{};
    std::int64_t pnl{0};  // the day's change
    std::uint64_t fills{0};

    bool operator==(const DayResult&) const = default;
};

// A run's results (spec §5.3); a failed or cancelled run's are its aggregates so far.
struct RunResult
{
    bool partial{false};  // failed or cancelled: its digests mean nothing
    bool cancelled{false};
    std::string reason{};  // a failed run's, e.g. "source_error on 2019-12-27: TruncatedMessage"
    std::uint64_t events{0};
    std::uint64_t sim_events{0};
    std::int64_t pnl{0};
    std::int64_t realised{0};
    std::int64_t unrealised{0};
    std::int64_t fees{0};
    std::int64_t position{0};
    std::int64_t max_abs_position{0};
    std::int64_t max_drawdown{0};  // Portfolio::max_drawdown(), exact at every mark
    std::uint64_t maker_fills{0};
    std::uint64_t taker_fills{0};
    std::uint64_t filled_shares{0};
    double fill_rate{0};  // filled over filled, cancelled and expired shares, as delivered
    Digests digests{};
    std::vector<Sample> equity{};
    std::map<int, std::int64_t> by_hour{};  // each sample's PnL change, by its New York hour
    std::vector<DayResult> by_day{};
    FillAttribution fills_by{};
    FillAttribution anomalies_by{};
    std::vector<FillRow> fills{};  // the first max_fill_rows
    bool truncated{false};
    std::vector<std::string> hashes{};  // with hash as read: each day's re-hash
    std::int64_t wall_ms{0};
    std::int64_t cpu_ms{0};

    bool operator==(const RunResult&) const = default;
};

// Runs `spec` over `days` (the days spec.days names, in order) to the end, or to `cancel`; bytes_read reports
// progress. No threads, no ReplaySession.
RunResult backtest(const SessionConfig& config, const RunSpec& spec, std::span<const DayHandle> days, bool hash_as_read,
                   const std::atomic<bool>& cancel, std::atomic<std::uint64_t>& bytes_read);

}  // namespace simulator::server
