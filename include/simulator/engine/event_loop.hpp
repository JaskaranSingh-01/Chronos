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

// Caps the end-of-stream drain, so a strategy that answers every report with an order still ends.
inline constexpr std::uint64_t max_drain_events = 100'000;

struct LoopReport
{
    ReplayReport replay;                    // market side, counted as MarketReplay::run counts it
    std::uint64_t sim_events{0};            // handled, drained ones included
    std::uint64_t drained_sim_events{0};    // handled after the market stream ended
    std::uint64_t discarded_sim_events{0};  // never handled: queued at a fatal failure or past the drain cap

    bool operator==(const LoopReport&) const = default;
};

// Merges the historical market stream with our SimQueue in timestamp order and drives the strategy.
// At end of stream it drains the SimQueue with timers disabled; a fatal failure discards it instead.
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
    std::vector<ExecutionReport> reports_;  // ExecutionSimulator output, scheduled and cleared after each call

    void schedule_reports(Timestamp now) noexcept
    {
        for (const ExecutionReport& report : reports_) {
            queue_.push(SimEvent{
                .timestamp = now + latency_.report_latency, .type = SimEventType::ReportDeliver, .report = report});
        }
        reports_.clear();
    }

    ApplyResult on_market(const MarketEvent& event) noexcept
    {
        gateway_.set_now(event.timestamp);
        execution_.on_market(event, book_, reports_);  // sees the pre-event book
        schedule_reports(event.timestamp);

        const ApplyResult applied = book_.apply(event);  // the only writer of the historical book
        if (portfolio_.position() != 0) {
            portfolio_.mark(book_);  // a flat pnl does not depend on the mark
        }
        if (applied == ApplyResult::Applied) {
            strategy_.on_market(context_, event);
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
        : source_(source),
          book_(book),
          execution_(execution),
          portfolio_(portfolio),
          strategy_(strategy),
          latency_(latency),
          options_(options),
          queue_(queue_capacity),
          gateway_(queue_, portfolio, latency),
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

            // SimEvents strictly before this market event; ties go to the market.
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

            if (!book_.check_invariants(replay.invariant_error, in_rth(event.timestamp, options_.session_midnight))) {
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
