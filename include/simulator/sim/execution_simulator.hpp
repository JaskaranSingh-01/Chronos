#pragma once

#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/sim/execution_report.hpp"

namespace simulator {

// Our orders against the historical book: the fill model (spec §4.10, D2).
// It reads the book through const MarketBook& and never mutates it, so
// historical replay and our orders never mix (D1).
//
// Increment 1, aggressive IOC only. On arrival an IOC walks the visible
// opposite levels best first while they cross (buy: level <= limit, sell:
// level >= limit), fills min(remaining, level quantity) at each level's price
// as Taker, and expires any leftover (leaves 0). The visible book is the state
// after every market event stamped <= arrival, since market events win ties.
//
// Known optimism (spec §11 Q2, accepted): fills take no liquidity, so repeated
// IOCs can take the same visible shares again, and an IOC never moves prices.
//
// Day (passive, post-only) orders are Rejected/UnsupportedTimeInForce until
// increment 2 adds resting orders and queue position.
class ExecutionSimulator
{
public:
    // Spec step ①, called before book.apply(event). No-op until increment 2:
    // an IOC never rests, so there is nothing for a market event to fill.
    void on_market(const MarketEvent&, const MarketBook&, std::vector<ExecutionReport>&) noexcept {}

    // Appends the order's reports to `out`; the last one has leaves 0.
    void on_order_arrive(const OrderRequest& request, Timestamp now, const MarketBook& book,
                         std::vector<ExecutionReport>& out) noexcept;
};

} // namespace simulator
