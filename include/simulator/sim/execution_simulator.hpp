#pragma once

#include <array>
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

// Why a resting order filled: the rule and the historical message behind it.
// An Execute with a price is ITCH C, without one ITCH E.
enum class FillCause : std::uint8_t {
    RuleAExecute,           // (a): an order behind us at our price executed
    RuleAExecuteWithPrice,
    RuleDExecute,           // (d): an order at a worse price executed
    RuleDExecuteWithPrice,
    RuleCAdd,               // (c): a crossing Add
    RuleCReplace            // (c): a crossing replacement
};
inline constexpr std::size_t fill_cause_count = 6;

// Exchange-local session phase; a cross is the first second at 09:30 or 16:00.
enum class SessionPhase : std::uint8_t { PreOpen, OpeningCross, Regular, ClosingCross, PostClose };
inline constexpr std::size_t session_phase_count = 5;

constexpr SessionPhase session_phase(Timestamp now, Timestamp session_midnight) noexcept
{
    constexpr Timestamp second = 1'000'000'000;
    const Timestamp t = now - session_midnight;
    return t < RTH_OPEN_NS            ? SessionPhase::PreOpen
         : t < RTH_OPEN_NS + second  ? SessionPhase::OpeningCross
         : t < RTH_CLOSE_NS          ? SessionPhase::Regular
         : t < RTH_CLOSE_NS + second ? SessionPhase::ClosingCross
                                     : SessionPhase::PostClose;
}

// Counts by [FillCause][SessionPhase].
using FillAttribution = std::array<std::array<std::uint64_t, session_phase_count>, fill_cause_count>;

// One FIFO anomaly with its context, for diagnosis.
struct FifoAnomaly
{
    MarketEvent event;             // the historical Execute
    Price h_price{};               // its order's displayed price, side and sequence, pre-event
    Side h_side{Side::Buy};
    std::uint64_t h_sequence{0};
    std::uint64_t h_remaining{0};
    ShadowOrder order;             // ours, before the fill
    std::uint64_t level_quantity{0};  // historical shares at our price, pre-event
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
    FillAttribution fills_by{};            // maker_fills by cause and phase
    FillAttribution anomalies_by{};        // fifo_anomalies likewise

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
//     §11 Q1: a trade-through would have hit o first): o fills at its price,
//     unless the Execute carries a trade price (ITCH C) better than o's. Such
//     an h is price-slid: displayed at o's price or worse, it ranks and trades
//     at a better, non-displayed one, so the aggressor never reached o.
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
    Timestamp session_midnight_{0};
    std::vector<FifoAnomaly> anomalies_;  // the first max_fifo_anomaly_samples

    void fill(ShadowOrder& order, std::uint64_t quantity, Timestamp now, FillCause cause,
              std::vector<ExecutionReport>& out) noexcept;
    // Rule (c) for a historical order of `side` at `price` for `quantity`.
    void cross(Side side, Price price, std::uint64_t quantity, Timestamp now, FillCause cause,
               std::vector<ExecutionReport>& out) noexcept;

public:
    static constexpr std::size_t max_fifo_anomaly_samples = 10;

    // session_midnight only places fills in a SessionPhase.
    explicit ExecutionSimulator(Timestamp session_midnight = 0)
        : session_midnight_(session_midnight)
    {
        anomalies_.reserve(max_fifo_anomaly_samples);
    }

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
    const std::vector<FifoAnomaly>& fifo_anomaly_samples() const noexcept { return anomalies_; }
};

} // namespace simulator
