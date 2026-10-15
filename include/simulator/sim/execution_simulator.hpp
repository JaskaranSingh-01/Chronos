#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "simulator/market/market_book.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/sim/execution_report.hpp"

namespace simulator {

// One of our resting Day orders.
struct ShadowOrder
{
    OrderId id{0};
    Side side{Side::Buy};
    Price price{};
    std::uint64_t remaining{0};
    std::uint64_t anchor_sequence{0};  // book.last_sequence() at arrival
    std::uint64_t ahead_quantity{0};   // historical shares ahead of us at our price
};

// The fill rule and historical message behind a resting-order fill.
// An Execute with a price is ITCH C, without one ITCH E.
enum class FillCause : std::uint8_t {
    RuleAExecute,
    RuleAExecuteWithPrice,
    RuleDExecute,
    RuleDExecuteWithPrice,
    RuleCAdd,
    RuleCReplace
};
inline constexpr std::size_t fill_cause_count = 6;

// Exchange-local session phase; each cross is the first second after 09:30 or 16:00.
enum class SessionPhase : std::uint8_t {
    PreOpen,
    OpeningCross,
    Regular,
    ClosingCross,
    PostClose
};
inline constexpr std::size_t session_phase_count = 5;

constexpr SessionPhase session_phase(Timestamp now, Timestamp session_midnight) noexcept
{
    constexpr Timestamp second = 1'000'000'000;
    const Timestamp t = now - session_midnight;
    return t < RTH_OPEN_NS          ? SessionPhase::PreOpen
        : t < RTH_OPEN_NS + second  ? SessionPhase::OpeningCross
        : t < RTH_CLOSE_NS          ? SessionPhase::Regular
        : t < RTH_CLOSE_NS + second ? SessionPhase::ClosingCross
                                    : SessionPhase::PostClose;
}

// Counts indexed [FillCause][SessionPhase].
using FillAttribution = std::array<std::array<std::uint64_t, session_phase_count>, fill_cause_count>;

// A sampled FIFO anomaly with the pre-event state that produced it.
struct FifoAnomaly
{
    MarketEvent event;                // the historical Execute
    Price h_price{};                  // the executed order's displayed price, side, sequence and size
    Side h_side{Side::Buy};
    std::uint64_t h_sequence{0};
    std::uint64_t h_remaining{0};
    ShadowOrder order;                // ours, before the fill
    std::uint64_t level_quantity{0};  // historical shares at our price
};

struct ExecutionStatistics
{
    std::uint64_t maker_fills{0};          // Fill reports of resting orders
    std::uint64_t maker_quantity{0};
    std::uint64_t taker_fills{0};          // Fill reports of IOCs
    std::uint64_t taker_quantity{0};
    std::uint64_t accepted_quantity{0};    // shares of Day orders that rested
    std::uint64_t would_cross_rejects{0};
    std::uint64_t not_trading_rejects{0};
    std::uint64_t cancelled{0};
    std::uint64_t cancel_rejects{0};
    std::uint64_t fifo_anomalies{0};       // rule (a) or (d) filled us while ahead_quantity > 0
    FillAttribution fills_by{};            // maker_fills by cause and phase
    FillAttribution anomalies_by{};        // fifo_anomalies by cause and phase

    bool operator==(const ExecutionStatistics&) const = default;
};

// Fills our orders against the historical book, which it reads but never mutates.
// IOCs take the visible crossing levels, best first, at each level's price, as Taker.
// A Day order rests behind every historical share already at its price.
// Resting orders fill as Maker at their own price, in priority order, capped at the event's quantity:
// (a) a historical execution against an order queued behind ours fills us first, at our price.
// (b) an execute, cancel, delete or replace of an order queued ahead of ours shrinks ahead_quantity.
// (c) an opposite-side Add or replacement that crosses our price fills us at our price.
// (d) a historical execution at a worse price than ours fills us first, at our price, as a trade-through would.
// Trade-price guard: (a) and (d) skip an Execute whose trade price beats ours; that order was price-slid.
// No market impact: our fills remove no historical liquidity, so IOCs can take the same shares again.
// While the book is not Trading only rule (b) runs, and new orders are rejected NotTrading.
class ExecutionSimulator
{
private:
    std::vector<ShadowOrder> orders_;  // per side, in priority order
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

    // session_midnight is used only to attribute fills to a SessionPhase.
    explicit ExecutionSimulator(Timestamp session_midnight = 0)
        : session_midnight_(session_midnight)
    {
        anomalies_.reserve(max_fifo_anomaly_samples);
    }

    // Call before book.apply(event). Appends our fills to `out`.
    void on_market(const MarketEvent& event, const MarketBook& book, std::vector<ExecutionReport>& out) noexcept;

    // Appends the order's reports to `out`: an IOC's last one has leaves 0,
    // a Day order gets Accepted or Rejected.
    void on_order_arrive(const OrderRequest& request, Timestamp now, const MarketBook& book,
                         std::vector<ExecutionReport>& out) noexcept;

    // Appends Cancelled for the open remainder, or CancelRejected if the order is no longer resting.
    void on_cancel_arrive(OrderId id, Timestamp now, std::vector<ExecutionReport>& out) noexcept;

    // nullptr if not resting. Invalidated by the next call that changes orders.
    const ShadowOrder* find(OrderId id) const noexcept;
    // Per side, in priority order; invalidated by the next call that changes orders.
    std::span<const ShadowOrder> resting_orders() const noexcept
    {
        return orders_;
    }
    std::size_t open_orders() const noexcept
    {
        return orders_.size();
    }
    const ExecutionStatistics& statistics() const noexcept
    {
        return statistics_;
    }
    const std::vector<FifoAnomaly>& fifo_anomaly_samples() const noexcept
    {
        return anomalies_;
    }
};

} // namespace simulator
