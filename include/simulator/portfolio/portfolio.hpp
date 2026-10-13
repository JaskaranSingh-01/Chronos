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

// Ticks per quantity unit, so fees are in cash's notional units. Negative is a rebate.
struct FeeSchedule
{
    std::int64_t maker_fee_per_share{0};
    std::int64_t taker_fee_per_share{0};
};

// Our position and money in one instrument, updated as each report is delivered to the strategy.
// Money is int64 notional units of 10^-(price_scale + quantity_scale) currency: tick-shares for a US equity.
class Portfolio
{
private:
    RiskLimits limits_;
    FeeSchedule fee_schedule_;

    std::int64_t position_{0};
    std::int64_t cash_{0};
    std::int64_t fees_{0};
    std::int64_t cost_{0};          // cost basis of the open position; negative when short
    Price bid_{};                   // last non-empty best bid and ask; 0 before any
    Price ask_{};
    std::int64_t peak_pnl_{0};
    std::int64_t max_drawdown_{0};
    std::int64_t max_abs_position_{0};

    std::size_t open_orders_{0};
    std::uint64_t open_buy_{0};     // reserved quantity, not yet filled or released
    std::uint64_t open_sell_{0};

    void apply_fill(const ExecutionReport& report) noexcept;
    void release(Side side, Quantity quantity, bool order_done) noexcept;

public:
    // Asserts price_scale + quantity_scale <= 12, so every price x quantity fits int64.
    Portfolio(const Instrument& instrument, RiskLimits limits, FeeSchedule fees) noexcept;

    // Pre-trade check in RiskCheck order; the position limit assumes every open same-side order fills.
    // Only Accepted changes state: it reserves the order.
    RiskCheck reserve(Side side, Quantity quantity) noexcept;

    // Fill moves position, cash and fees; Fill with leaves 0, Cancelled, Expired and Rejected close the order.
    // Precondition: the order was reserved.
    void on_report(const ExecutionReport& report) noexcept;

    // Records the best bid and ask and samples drawdown. Call after fills and,
    // while position() != 0, after market events.
    void mark(const MarketBook& book) noexcept;

    std::int64_t position() const noexcept { return position_; }  // quantity units
    std::int64_t cash() const noexcept { return cash_; }          // notional units
    std::int64_t fees() const noexcept { return fees_; }          // notional units
    std::int64_t realised_pnl() const noexcept;    // cash + cost
    std::int64_t unrealised_pnl() const noexcept;  // position x mark - cost; longs mark at the bid, shorts at the ask
    std::int64_t pnl() const noexcept;             // realised + unrealised - fees
    std::int64_t max_drawdown() const noexcept { return max_drawdown_; }  // max(peak pnl - pnl) over marks
    std::int64_t max_abs_position() const noexcept { return max_abs_position_; }

    std::size_t open_orders() const noexcept { return open_orders_; }
    std::uint64_t open_quantity(Side side) const noexcept
    {
        return side == Side::Buy ? open_buy_ : open_sell_;
    }
};

} // namespace simulator
