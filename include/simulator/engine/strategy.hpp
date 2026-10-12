#pragma once

#include <concepts>
#include <cstdint>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/portfolio/portfolio.hpp"
#include "simulator/sim/execution_report.hpp"
#include "simulator/sim/order_gateway.hpp"

namespace simulator {

// What a strategy callback may touch. The book and the portfolio are
// read-only; every action goes through the gateway, so a strategy can never
// change historical state (D1).
struct StrategyContext
{
    const MarketBook& book;
    const Portfolio& portfolio;
    OrderGateway& gateway;

    Timestamp now() const noexcept { return gateway.now(); }

    // See OrderGateway::set_timer.
    bool set_timer(Timestamp at, std::uint64_t timer_id) const noexcept { return gateway.set_timer(at, timer_id); }
};

// Spec §4.11, with the context in place of (book, gateway). on_market runs
// only for events the book Applied (Trade included) and sees the post-event
// book; on_report runs at delivery, after the Portfolio has the report.
// A strategy keeps its own state and calls the gateway only inside callbacks.
template <class S>
concept Strategy = requires(S& s, StrategyContext& ctx, const MarketEvent& event, const ExecutionReport& report,
                            std::uint64_t timer_id) {
    { s.on_market(ctx, event) } noexcept -> std::same_as<void>;
    { s.on_report(ctx, report) } noexcept -> std::same_as<void>;
    { s.on_timer(ctx, timer_id) } noexcept -> std::same_as<void>;
};

// Replay only: the callbacks inline away and the SimQueue stays empty.
struct NullStrategy
{
    void on_market(StrategyContext&, const MarketEvent&) noexcept {}
    void on_report(StrategyContext&, const ExecutionReport&) noexcept {}
    void on_timer(StrategyContext&, std::uint64_t) noexcept {}
};

static_assert(Strategy<NullStrategy>);

} // namespace simulator
