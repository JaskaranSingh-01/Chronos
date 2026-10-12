#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/sim/execution_report.hpp"

namespace simulator {

// One of our resting Day orders, as the fill rules see it.
struct ShadowOrder
{
    OrderId id{0};
    Side side{Side::Buy};
    Price price{};
    std::uint64_t remaining{0};
    std::uint64_t anchor_sequence{0};  // book.last_sequence() at arrival
    std::uint64_t ahead_quantity{0};   // historical shares ahead of us at our price
};

struct ExecutionStatistics
{
    std::uint64_t maker_fills{0};          // Fill reports of resting orders
    std::uint64_t maker_quantity{0};
    std::uint64_t taker_fills{0};          // Fill reports of IOCs
    std::uint64_t taker_quantity{0};
    std::uint64_t accepted_quantity{0};    // Day orders that rested
    std::uint64_t would_cross_rejects{0};
    std::uint64_t cancelled{0};
    std::uint64_t cancel_rejects{0};
    std::uint64_t fifo_anomalies{0};       // rule (a) or (d) filled us while ahead_quantity > 0

    bool operator==(const ExecutionStatistics&) const = default;
};

// Our orders against the historical book: the fill model (spec §4.10, D2).
// It reads the book through const MarketBook& and never mutates it, so
// historical replay and our orders never mix (D1).
//
// IOC, at arrival: walk the visible opposite levels best first while they
// cross (buy: level <= limit, sell: level >= limit), fill min(remaining,
// level quantity) at each level's price as Taker, expire any leftover. The
// visible book is the state after every market event stamped <= arrival,
// since market events win ties. Known optimism (spec §11 Q2, accepted): fills
// take no liquidity, so repeated IOCs can take the same visible shares again.
//
// Day = post-only passive (§11 Q3). At arrival, Rejected/WouldCross if it
// would take liquidity (buy >= best ask, sell <= best bid); otherwise it rests
// virtually with anchor_sequence = book.last_sequence() and ahead_quantity =
// book.quantity_at(side, price), so every historical order now at our price
// is ahead of us, and is Accepted. A cancel removes what is left (Cancelled,
// leaves 0) or, if nothing is, gets CancelRejected; fills before it stand.
//
// on_market runs before book.apply(e), so h = book.find(e.order_id) is still
// the historical order's pre-event state. For our order o on h's side:
//   h at o's price with sequence <= anchor (ahead of o): an Execute, Cancel,
//     Delete or Replace removes its shares from ahead_quantity (rule b);
//   Execute of h at o's price behind o (rule a), or at a worse price (rule d,
//     §11 Q1: a trade-through would have hit o first): o fills at its price.
// An Add, or a Replace's replacement, that crosses o on the opposite side
// (sell at <= our bid, buy at >= our ask) fills o at its price (rule c).
// Every such fill is Maker. One event fills ours in our priority order (best
// price, then arrival), the total capped at the event's quantity; rule (b) is
// applied to each order on its own. Events the book will not apply (another
// symbol, unknown or duplicate ids, a bad quantity or replace) are ignored.
class ExecutionSimulator
{
private:
    // ponytail: linear scans over few live orders, kept in priority order per
    // side; per-price buckets if a strategy ever keeps hundreds live.
    std::vector<ShadowOrder> orders_;
    ExecutionStatistics statistics_{};

    void fill(ShadowOrder& order, std::uint64_t quantity, Timestamp now, std::vector<ExecutionReport>& out) noexcept;
    // Rule (c) for a historical order of `side` at `price` for `quantity`.
    void cross(Side side, Price price, std::uint64_t quantity, Timestamp now,
               std::vector<ExecutionReport>& out) noexcept;

public:
    // Spec step ①, called before book.apply(event). Appends our fills to `out`.
    void on_market(const MarketEvent& event, const MarketBook& book, std::vector<ExecutionReport>& out) noexcept;

    // Appends the order's reports to `out`: an IOC's last one has leaves 0,
    // a Day order gets Accepted or Rejected.
    void on_order_arrive(const OrderRequest& request, Timestamp now, const MarketBook& book,
                         std::vector<ExecutionReport>& out) noexcept;

    // Cancelled or CancelRejected.
    void on_cancel_arrive(OrderId id, Timestamp now, std::vector<ExecutionReport>& out) noexcept;

    // A resting order, or nullptr. Invalidated by the next call that changes orders.
    const ShadowOrder* find(OrderId id) const noexcept;
    std::size_t open_orders() const noexcept { return orders_.size(); }
    const ExecutionStatistics& statistics() const noexcept { return statistics_; }
};

} // namespace simulator
