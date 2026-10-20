#include "chronos/server/backtest.hpp"

#include <time.h>

#include <chrono>
#include <memory>
#include <utility>

#include "simulator/cli/replay_args.hpp"

namespace simulator::server {
namespace {

constexpr Timestamp second_ns = 1'000'000'000;
constexpr Timestamp hour_ns = 3'600 * second_ns;

std::int64_t thread_cpu_ns() noexcept
{
    timespec now{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return static_cast<std::int64_t>(now.tv_sec) * second_ns + now.tv_nsec;
}

// A failed run's reason; a source error names the day the chain had reached.
std::string failure(const ReplayReport& replay, std::span<const DayHandle> days, Timestamp midnight)
{
    switch (replay.failure) {
    case ReplayFailure::None:
        break;
    case ReplayFailure::SourceError: {
        std::size_t day = 0;
        while (day + 1 < days.size() && days[day + 1]->midnight <= midnight) {
            ++day;
        }
        return "source_error on " + iso_date(days[day]->date) + ": " + std::string{to_string(replay.source_error)};
    }
    case ReplayFailure::OutOfOrderTimestamp:
        return "out_of_order_timestamp";
    case ReplayFailure::InvariantViolation:
        return "invariant_violation";
    }
    return {};
}

}  // namespace

RiskLimits default_limits(const StrategySpec& strategy)
{
    const bool mm = strategy.kind == SessionStrategyKind::MarketMaker;
    return {.max_position = mm ? strategy.market_maker.max_position : strategy.imbalance.max_position,
            .max_order_quantity = Quantity{mm ? strategy.market_maker.quote_size : strategy.imbalance.clip},
            .max_open_orders = default_max_open_orders(mm ? StrategyKind::MarketMaker : StrategyKind::Imbalance)};
}

RunSpec default_spec(SessionStrategyKind kind)
{
    const BacktestArgs cli{};
    const StrategySpec strategy{
        .kind = kind,
        .market_maker = {.quote_size = cli.quote_size,
                         .max_position = cli.max_position,
                         .requote_cooldown_ns = static_cast<Timestamp>(cli.requote_ms) * 1'000'000},
        .imbalance = {.threshold = cli.threshold,
                      .clip = cli.clip,
                      .max_position = cli.max_position,
                      .cooldown_ns = static_cast<Timestamp>(cli.cooldown_ms) * 1'000'000}};
    return RunSpec{.latency = {.order_latency = static_cast<Timestamp>(cli.order_latency_ns),
                               .report_latency = static_cast<Timestamp>(cli.report_latency_ns)},
                   .fees = {.maker_fee_per_share = cli.maker_fee_ticks,
                            .taker_fee_per_share = static_cast<std::int64_t>(cli.taker_fee_ticks)},
                   .limits = default_limits(strategy),
                   .window = TradingWindow::Regular,
                   .strategy = strategy};
}

RunResult backtest(const SessionConfig& config, const RunSpec& spec, std::span<const DayHandle> days, bool hash_as_read,
                   const std::atomic<bool>& cancel, std::atomic<std::uint64_t>& bytes_read)
{
    const auto wall_start = std::chrono::steady_clock::now();
    const std::int64_t cpu_start = thread_cpu_ns();
    auto chain =
        std::make_unique<DaySequenceSource>(std::vector<DayHandle>(days.begin(), days.end()), name(spec.ticker),
                                            config.instrument, config.allow_truncated_tail, hash_as_read);
    const DaySequenceSource& source = *chain;
    Generation g{config, spec, std::move(chain)};
    RunResult r;
    std::int64_t position = 0;  // after each fill
    std::uint64_t filled = 0;
    std::uint64_t removed = 0;  // cancelled and expired shares
    std::int64_t day_start = 0;  // the PnL when the day began
    std::uint64_t day_fills = 0;
    std::int64_t sampled = 0;  // the PnL at the last sample
    std::int64_t second = -1;  // the exchange second of the last market event not yet sampled

    const auto drain = [&] {
        for (const ExecutionReport& report : g.strategy.delivered) {
            const auto quantity = static_cast<std::int64_t>(report.quantity.value());
            if (report.type == ReportType::Cancelled || report.type == ReportType::Expired) {
                removed += report.quantity.value();
            }
            if (report.type != ReportType::Fill) {
                continue;
            }
            position += report.side == Side::Buy ? quantity : -quantity;
            filled += report.quantity.value();
            ++day_fills;
            if (r.fills.size() == max_fill_rows) {
                r.truncated = true;
                continue;
            }
            r.fills.push_back({.exchange_ts = report.exchange_time,
                               .order_id = report.order_id,
                               .side = report.side,
                               .price = report.price,
                               .quantity = report.quantity,
                               .liquidity = report.liquidity,
                               .rule = report.cause,
                               .ahead_at_entry = report.entry_ahead_quantity,
                               .position = position});
        }
        g.strategy.delivered.clear();
    };
    const auto sample = [&] {
        const std::int64_t pnl = g.portfolio.pnl();
        r.equity.push_back({.second = second, .pnl = pnl, .position = g.portfolio.position()});
        r.by_hour[static_cast<int>((second * second_ns - g.loop.session_midnight()) / hour_ns)] += pnl - sampled;
        sampled = pnl;
        second = -1;
    };
    const auto end_day = [&] {
        r.by_day.push_back(
            {.date = days[r.by_day.size()]->date, .pnl = g.portfolio.pnl() - day_start, .fills = day_fills});
        day_start = g.portfolio.pnl();
        day_fills = 0;
    };

    Timestamp next{0};
    while (!cancel.load(std::memory_order_relaxed) && g.loop.peek(next) == PeekStatus::Event) {
        const bool clear = g.loop.pending()->type == MarketEventType::Clear;
        if (second >= 0 && next / second_ns > second) {  // the last event's second is over
            g.loop.advance_to((second + 1) * second_ns - 1);
            drain();
            sample();
        }
        if (clear) {  // the PnL at the end of the day, after its last simulator events
            g.loop.advance_to(next - 1);
            drain();
            end_day();
        }
        const bool stepped = step(g.loop).ok;
        drain();
        bytes_read.store(source.position(), std::memory_order_relaxed);
        if (!stepped) {
            break;
        }
        second = clear ? second : next / second_ns;
    }
    const ReplayReport& replay = g.loop.report().replay;
    r.cancelled =
        cancel.load(std::memory_order_relaxed) && replay.succeeded() && g.loop.peek(next) == PeekStatus::Event;
    if (!r.cancelled) {
        finish(g);
        drain();
    }
    if (second >= 0) {
        sample();
    }
    if (!days.empty()) {
        end_day();
    }

    const Portfolio& p = g.portfolio;
    const ExecutionStatistics& e = g.execution.statistics();
    r.reason = failure(replay, days, g.loop.session_midnight());
    r.partial = r.cancelled || !replay.succeeded();
    r.events = replay.events;
    r.sim_events = g.loop.report().sim_events;
    r.pnl = p.pnl();
    r.realised = p.realised_pnl();
    r.unrealised = p.unrealised_pnl();
    r.fees = p.fees();
    r.position = p.position();
    r.max_abs_position = p.max_abs_position();
    r.max_drawdown = p.max_drawdown();
    r.maker_fills = e.maker_fills;
    r.taker_fills = e.taker_fills;
    r.filled_shares = e.maker_quantity + e.taker_quantity;
    r.fill_rate = filled + removed == 0 ? 0.0 : static_cast<double>(filled) / static_cast<double>(filled + removed);
    r.digests = r.partial ? Digests{} : digests(g);
    r.fills_by = e.fills_by;
    r.anomalies_by = e.anomalies_by;
    bytes_read.store(source.position(), std::memory_order_relaxed);
    if (hash_as_read && !r.cancelled) {
        r.hashes = source.hashes();
        // ponytail: re-reads each unread day whole and blind to cancel; check cancel here if long chains make it slow.
        for (std::size_t i = r.hashes.size(); i < days.size(); ++i) {
            r.hashes.push_back(file_hash(days[i]->path));  // the replay stopped before it read the whole day
        }
    }
    r.wall_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wall_start).count();
    r.cpu_ms = (thread_cpu_ns() - cpu_start) / 1'000'000;
    return r;
}

}  // namespace simulator::server
