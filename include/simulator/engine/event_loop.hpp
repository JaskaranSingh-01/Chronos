#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "simulator/engine/strategy.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/sim/execution_simulator.hpp"
#include "simulator/sim/order_gateway.hpp"
#include "simulator/sim/sim_event.hpp"

namespace simulator {

// The most SimEvents the end-of-stream drain handles (see EventLoop).
inline constexpr std::uint64_t max_drain_events = 100'000;

struct LoopReport
{
    ReplayReport replay;                    // the market side, counted as MarketReplay::run counts it
    std::uint64_t sim_events{0};            // SimEvents handled, drained ones included
    std::uint64_t drained_sim_events{0};    // handled after the market stream's EndOfStream
    std::uint64_t discarded_sim_events{0};  // never handled: queued at a fatal failure, or past the drain cap

    bool operator==(const LoopReport&) const = default;
};

// The M3 loop (spec §4.8, §5): the historical market stream and our SimQueue,
// merged in (timestamp, stream, sequence) order with ties going to the market.
//
// Per market event e at t:
//   ① execution.on_market(e, book) on the pre-event book; its reports are
//     scheduled for delivery at t + report_latency
//   ② book.apply(e), the only writer of the historical book
//   ③ portfolio.mark(book) while the position is open (flat, pnl does not
//     depend on the mark, and every delivered report marks anyway)
//   ④ strategy.on_market, only if ② Applied, so it sees the post-event book
// Per SimEvent at t:
//   OrderArrive   execution.on_order_arrive; reports delivered at t + report_latency
//   CancelArrive  execution.on_cancel_arrive; likewise
//   ReportDeliver portfolio.on_report, portfolio.mark, then strategy.on_report
//   Timer         strategy.on_timer
//
// Unlike spec §5, the Portfolio takes each report at delivery rather than at
// exchange time, so the const Portfolio& a strategy reads never shows a fill
// before its report arrives. Prices and exchange_time are unchanged; only
// marks inside the report_latency window differ.
//
// End of stream: the SimQueue is drained in (timestamp, sequence) order, so
// orders, cancels and reports still in flight reach the ExecutionSimulator,
// the Portfolio and the strategy. The clock advances to each event; the book
// stays frozen at its final state. Callbacks still run and may send orders
// and cancels, but set_timer now returns false, so a re-arming timer ends.
// A strategy that answers every report with another order would never end, so
// the drain stops after max_drain_events; whatever is left is discarded and
// counted. Deterministic either way.
//
// Fatal, as in MarketReplay::run: a source Error, a market timestamp earlier
// than the one before, or a failed periodic invariant check. A fatal failure
// ends the run at once: queued SimEvents are discarded and counted, not
// drained. No wall clock: callers time run().
template <Strategy S>
class EventLoop
{
private:
    MarketEventSource& source_;
    MarketBook& book_;
    ExecutionSimulator& execution_;
    Portfolio& portfolio_;
    S& strategy_;
    LatencyConfig latency_;
    ReplayOptions options_;
    SimQueue queue_;
    OrderGateway gateway_;
    StrategyContext context_;
    std::vector<ExecutionReport> reports_;  // ExecutionSimulator output, drained after each call

    void schedule_reports(Timestamp now) noexcept
    {
        for (const ExecutionReport& report : reports_) {
            queue_.push(SimEvent{.timestamp = now + latency_.report_latency, .type = SimEventType::ReportDeliver,
                                 .report = report});
        }
        reports_.clear();
    }

    ApplyResult on_market(const MarketEvent& event) noexcept
    {
        gateway_.set_now(event.timestamp);
        execution_.on_market(event, book_, reports_);  // ①
        schedule_reports(event.timestamp);

        const ApplyResult applied = book_.apply(event);  // ②
        if (portfolio_.position() != 0) {
            portfolio_.mark(book_);  // ③
        }
        if (applied == ApplyResult::Applied) {
            strategy_.on_market(context_, event);  // ④
        }
        return applied;
    }

    void on_sim(const SimEvent& sim) noexcept
    {
        gateway_.set_now(sim.timestamp);
        switch (sim.type) {
        case SimEventType::OrderArrive:
            execution_.on_order_arrive(sim.order, sim.timestamp, book_, reports_);
            schedule_reports(sim.timestamp);
            return;
        case SimEventType::CancelArrive:
            execution_.on_cancel_arrive(sim.order_id, sim.timestamp, reports_);
            schedule_reports(sim.timestamp);
            return;
        case SimEventType::ReportDeliver:
            portfolio_.on_report(sim.report);
            portfolio_.mark(book_);
            strategy_.on_report(context_, sim.report);
            return;
        case SimEventType::Timer:
            strategy_.on_timer(context_, sim.timer_id);
            return;
        }
    }

public:
    EventLoop(MarketEventSource& source, MarketBook& book, ExecutionSimulator& execution, Portfolio& portfolio,
              S& strategy, LatencyConfig latency, ReplayOptions options = {}, std::size_t queue_capacity = 1'024)
        : source_(source), book_(book), execution_(execution), portfolio_(portfolio), strategy_(strategy),
          latency_(latency), options_(options), queue_(queue_capacity), gateway_(queue_, portfolio, latency),
          context_{book, portfolio, gateway_}
    {
        reports_.reserve(64);
    }

    // Runs to the end of the market stream or its first fatal failure. Call once.
    LoopReport run() noexcept
    {
        LoopReport report;
        ReplayReport& replay = report.replay;
        MarketEvent event;
        std::uint64_t until_check = options_.check_invariants_every;

        auto fail = [&](ReplayFailure failure, std::string_view message) {
            replay.failure = failure;
            replay.failed_position = source_.position();
            replay.message = message;
        };

        for (;;) {
            const SourceResult result = source_.next(event);

            if (result == SourceResult::EndOfStream) {
                gateway_.stop_timers();
                for (; !queue_.empty() && report.drained_sim_events < max_drain_events; ++report.drained_sim_events) {
                    const SimEvent sim = queue_.top();  // a copy: handling it may push
                    queue_.pop();
                    ++report.sim_events;
                    on_sim(sim);
                }
                break;
            }
            if (result == SourceResult::Error) {
                replay.source_error = source_.error();
                fail(ReplayFailure::SourceError, to_string(replay.source_error));
                break;
            }

            if (replay.events == 0) {
                replay.first_timestamp = event.timestamp;
            } else if (event.timestamp < replay.last_timestamp) {
                fail(ReplayFailure::OutOfOrderTimestamp, "timestamp earlier than the previous event's");
                break;
            }
            replay.last_timestamp = event.timestamp;
            ++replay.events;

            // Every SimEvent strictly before this market event; a tie goes to the market.
            while (!queue_.empty() && queue_.top().timestamp < event.timestamp) {
                const SimEvent sim = queue_.top();  // a copy: handling it may push
                queue_.pop();
                ++report.sim_events;
                on_sim(sim);
            }

            const ApplyResult applied = on_market(event);
            ++replay.by_type[static_cast<std::size_t>(event.type)];
            ++replay.by_result[static_cast<std::size_t>(applied)];

            if (applied != ApplyResult::Applied || until_check == 0 || --until_check != 0) {
                continue;
            }
            until_check = options_.check_invariants_every;
            ++replay.invariant_checks;

            if (!book_.check_invariants(replay.invariant_error,
                                        in_rth(event.timestamp, options_.session_midnight))) {
                fail(ReplayFailure::InvariantViolation, to_string(replay.invariant_error));
                break;
            }
        }

        for (; !queue_.empty(); queue_.pop()) {
            ++report.discarded_sim_events;
        }
        return report;
    }
};

} // namespace simulator
