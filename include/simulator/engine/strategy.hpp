#pragma once

#include <concepts>
#include <cstdint>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/portfolio/portfolio.hpp"
#include "simulator/sim/execution_report.hpp"
#include "simulator/sim/order_gateway.hpp"

namespace simulator {

// What a strategy callback may touch: a read-only book and portfolio, and the gateway for every action.
struct StrategyContext
{
    const MarketBook& book;
    const Portfolio& portfolio;
    OrderGateway& gateway;

    Timestamp now() const noexcept { return gateway.now(); }
    bool set_timer(Timestamp at, std::uint64_t timer_id) const noexcept { return gateway.set_timer(at, timer_id); }
};

// on_market runs only for events the book applied and sees the post-event book; on_report runs
// after the Portfolio has the report. A strategy calls the gateway only inside callbacks.
template <class S>
concept Strategy = requires(S& s, StrategyContext& ctx, const MarketEvent& event, const ExecutionReport& report,
                            std::uint64_t timer_id) {
    { s.on_market(ctx, event) } noexcept -> std::same_as<void>;
    { s.on_report(ctx, report) } noexcept -> std::same_as<void>;
    { s.on_timer(ctx, timer_id) } noexcept -> std::same_as<void>;
};

// Does nothing; for pure market replay.
struct NullStrategy
{
    void on_market(StrategyContext&, const MarketEvent&) noexcept {}
    void on_report(StrategyContext&, const ExecutionReport&) noexcept {}
    void on_timer(StrategyContext&, std::uint64_t) noexcept {}
};

static_assert(Strategy<NullStrategy>);

} // namespace simulator
