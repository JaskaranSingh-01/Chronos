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

enum class PeekStatus : std::uint8_t {
    Event,
    EndOfStream,
    Error
};

struct StepResult
{
    ApplyResult applied{ApplyResult::Applied};
    bool ok{false};  // false: there was no pending event, or a fatal failure ended the stream
};

struct LoopReport
{
    ReplayReport replay;                    // market side, counted as MarketReplay::run counts it
    std::uint64_t sim_events{0};            // handled, drained ones included
    std::uint64_t drained_sim_events{0};    // handled by finish()
    std::uint64_t discarded_sim_events{0};  // never handled: queued at a fatal failure or past the drain cap

    bool operator==(const LoopReport&) const = default;
};

// Merges the historical market stream with our SimQueue in (timestamp, class, sequence) order and
// drives the strategy: run() to the end, or step by step with peek/advance_to/step/finish.
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
    LoopReport report_;
    MarketEvent pending_{};  // read by peek(), applied by step()
    bool has_pending_{false};
    bool ended_{false};    // the source returned EndOfStream
    bool failed_{false};   // a fatal failure ended the stream
    bool finished_{false};
    std::uint64_t until_check_{0};

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

    void fail(ReplayFailure failure, std::string_view message) noexcept
    {
        ReplayReport& replay = report_.replay;
        replay.failure = failure;
        replay.failed_position = source_.position();
        replay.message = message;
        failed_ = true;
        has_pending_ = false;
    }

    void run_next_sim() noexcept
    {
        const SimEvent sim = queue_.top();  // a copy: handling it may push
        queue_.pop();
        ++report_.sim_events;
        on_sim(sim);
    }

public:
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

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
          context_{book, portfolio, gateway_},
          until_check_(options.check_invariants_every)
    {
        reports_.reserve(64);
    }

    // Reads the next market event into a one-event lookahead; repeated calls return the same event.
    PeekStatus peek(Timestamp& next_ts) noexcept
    {
        if (failed_) {
            return PeekStatus::Error;
        }
        if (!has_pending_) {
            if (ended_ || finished_) {
                return PeekStatus::EndOfStream;
            }
            const SourceResult result = source_.next(pending_);
            if (result == SourceResult::EndOfStream) {
                ended_ = true;
                gateway_.stop_timers();
                return PeekStatus::EndOfStream;
            }
            ReplayReport& replay = report_.replay;
            if (result == SourceResult::Error) {
                replay.source_error = source_.error();
                fail(ReplayFailure::SourceError, to_string(replay.source_error));
                return PeekStatus::Error;
            }
            if (replay.events != 0 && pending_.timestamp < replay.last_timestamp) {
                fail(ReplayFailure::OutOfOrderTimestamp, "timestamp earlier than the previous event's");
                return PeekStatus::Error;
            }
            has_pending_ = true;
        }
        next_ts = pending_.timestamp;
        return PeekStatus::Event;
    }

    // Peeks, runs the SimEvents due by t ahead of the next market event, then sets the clock to t if that is before it.
    void advance_to(Timestamp t) noexcept
    {
        Timestamp next_ts{0};
        if (failed_ || finished_ || t < gateway_.now() || peek(next_ts) == PeekStatus::Error) {
            return;
        }
        while (!queue_.empty()) {
            const Timestamp ts = queue_.top().timestamp;
            if (ts > t || (has_pending_ && ts >= pending_.timestamp)) {
                break;
            }
            run_next_sim();
        }
        if (!has_pending_ || t < pending_.timestamp) {
            gateway_.set_now(t);
        }
    }

    // Applies the pending market event after the SimEvents strictly before it; those at its timestamp wait.
    StepResult step() noexcept
    {
        if (!has_pending_) {
            return {};
        }
        while (!queue_.empty() && queue_.top().timestamp < pending_.timestamp) {
            run_next_sim();
        }
        has_pending_ = false;
        ReplayReport& replay = report_.replay;
        if (replay.events == 0) {
            replay.first_timestamp = pending_.timestamp;
        }
        replay.last_timestamp = pending_.timestamp;
        ++replay.events;
        const ApplyResult applied = on_market(pending_);
        ++replay.by_type[static_cast<std::size_t>(pending_.type)];
        ++replay.by_result[static_cast<std::size_t>(applied)];

        if (applied == ApplyResult::Applied && until_check_ != 0 && --until_check_ == 0) {
            until_check_ = options_.check_invariants_every;
            ++replay.invariant_checks;
            if (!book_.check_invariants(replay.invariant_error,
                                        in_rth(pending_.timestamp, options_.session_midnight))) {
                fail(ReplayFailure::InvariantViolation, to_string(replay.invariant_error));
                return {applied, false};
            }
        }
        return {applied, true};
    }

    // Drains the SimQueue after EndOfStream (capped at max_drain_events) and discards the rest; idempotent.
    void finish() noexcept
    {
        if (finished_) {
            return;
        }
        finished_ = true;
        has_pending_ = false;
        if (ended_ && !failed_) {
            for (; !queue_.empty() && report_.drained_sim_events < max_drain_events; ++report_.drained_sim_events) {
                run_next_sim();
            }
        }
        for (; !queue_.empty(); queue_.pop()) {
            ++report_.discarded_sim_events;
        }
    }

    // Runs to the end of the market stream or its first fatal failure.
    LoopReport run() noexcept
    {
        Timestamp next_ts{0};
        while (peek(next_ts) == PeekStatus::Event && step().ok) {
        }
        finish();
        return report_;
    }

    const LoopReport& report() const noexcept
    {
        return report_;
    }

    // When the next SimEvent is due; false if none is queued.
    bool next_sim_time(Timestamp& ts) const noexcept
    {
        if (queue_.empty()) {
            return false;
        }
        ts = queue_.top().timestamp;
        return true;
    }

    // The market event peek() read and step() applies next; nullptr if none is pending.
    const MarketEvent* pending() const noexcept
    {
        return has_pending_ ? &pending_ : nullptr;
    }

    OrderGateway& gateway() noexcept
    {
        return gateway_;
    }

    const MarketBook& book() const noexcept
    {
        return book_;
    }

    const Portfolio& portfolio() const noexcept
    {
        return portfolio_;
    }

    const ExecutionSimulator& execution() const noexcept
    {
        return execution_;
    }
};

}  // namespace simulator
