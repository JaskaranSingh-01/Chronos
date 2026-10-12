#pragma once

#include <cstddef>
#include <cstdint>

#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/sim/execution_report.hpp"

namespace simulator {

struct RiskLimits
{
    std::int64_t max_position{0};          // |worst-case position|, quantity units
    Quantity max_order_quantity{0};
    std::size_t max_open_orders{0};
};

// Ticks per quantity unit, so a fee is in the same notional units as cash.
// Negative = rebate.
struct FeeSchedule
{
    std::int64_t maker_fee_per_share{0};
    std::int64_t taker_fee_per_share{0};
};

// Our position and money in one instrument. The EventLoop feeds it each
// report when the report is delivered, report_latency after exchange_time,
// just before the strategy sees it, so what a strategy reads is never ahead
// of what it has been told.
//
// Money is int64 notional units of 10^-(price_scale + quantity_scale) of the
// instrument's currency: tick-shares ($0.0001) for a US equity. Every
// price x quantity product is taken in __int128 and must fit int64; the
// constructor asserts price_scale + quantity_scale <= 12, which leaves
// +-9.2 million currency units.
//
// pnl() = realised_pnl() + unrealised_pnl() - fees(), where
//   unrealised = position x mark - cost of the open position (average cost),
//   realised   = cash + cost of the open position.
// The mark is conservative liquidation value: a long at the best bid, a short
// at the best ask, the last one seen if that side is empty (0 before any).
class Portfolio
{
private:
    RiskLimits limits_;
    FeeSchedule fee_schedule_;

    std::int64_t position_{0};
    std::int64_t cash_{0};
    std::int64_t fees_{0};
    std::int64_t cost_{0};          // signed: positive long, negative short
    Price bid_{};                   // last marks
    Price ask_{};
    std::int64_t peak_pnl_{0};
    std::int64_t max_drawdown_{0};
    std::int64_t max_abs_position_{0};

    std::size_t open_orders_{0};
    std::uint64_t open_buy_{0};     // reserved, not yet filled or released
    std::uint64_t open_sell_{0};

    void apply_fill(const ExecutionReport& report) noexcept;
    void release(Side side, Quantity quantity, bool order_done) noexcept;

public:
    Portfolio(const Instrument& instrument, RiskLimits limits, FeeSchedule fees) noexcept;

    // Pre-trade, called by OrderGateway::send. Checks in RiskCheck order: zero
    // quantity, max_order_quantity, max_open_orders, then the worst-case
    // position if every open same-side order filled. Accepted reserves the
    // order; nothing else changes state.
    RiskCheck reserve(Side side, Quantity quantity) noexcept;

    // At delivery (see above). Fill moves position, cash and fees and consumes the
    // reservation (leaves == 0 closes the order); Cancelled, Expired and
    // Rejected release `quantity` and close the order. Every order these
    // reports name must have been reserved.
    void on_report(const ExecutionReport& report) noexcept;

    // Records the book's best bid and ask, then samples drawdown. pnl() uses
    // the last mark, so call it after each batch of fills as well as after
    // market events while position() != 0.
    void mark(const MarketBook& book) noexcept;

    std::int64_t position() const noexcept { return position_; }  // quantity units
    std::int64_t cash() const noexcept { return cash_; }          // notional units
    std::int64_t fees() const noexcept { return fees_; }          // notional units
    std::int64_t realised_pnl() const noexcept;
    std::int64_t unrealised_pnl() const noexcept;
    std::int64_t pnl() const noexcept;
    std::int64_t max_drawdown() const noexcept { return max_drawdown_; }  // max(peak pnl - pnl) over marks
    std::int64_t max_abs_position() const noexcept { return max_abs_position_; }

    std::size_t open_orders() const noexcept { return open_orders_; }
    std::uint64_t open_quantity(Side side) const noexcept
    {
        return side == Side::Buy ? open_buy_ : open_sell_;
    }
};

} // namespace simulator
