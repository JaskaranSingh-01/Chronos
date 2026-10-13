#include "simulator/portfolio/portfolio.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>

namespace simulator {
namespace {

__extension__ using Int128 = __int128;

// Narrows money computed in __int128 to int64; a value that does not fit is a logic bug.
std::int64_t to_money(Int128 value) noexcept
{
    assert(value >= std::numeric_limits<std::int64_t>::min() && value <= std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(value);
}

} // namespace

Portfolio::Portfolio([[maybe_unused]] const Instrument& instrument, RiskLimits limits, FeeSchedule fees) noexcept
    : limits_(limits), fee_schedule_(fees)
{
    assert(instrument.price_scale + instrument.quantity_scale <= 12);
}

RiskCheck Portfolio::reserve(Side side, Quantity quantity) noexcept
{
    if (quantity.is_zero()) {
        return RiskCheck::InvalidOrder;
    }
    if (quantity > limits_.max_order_quantity) {
        return RiskCheck::OrderTooLarge;
    }
    if (open_orders_ >= limits_.max_open_orders) {
        return RiskCheck::TooManyOpenOrders;
    }

    // Worst case: every open order on this side fills, this one included.
    const Int128 worst = side == Side::Buy ? Int128{position_} + open_buy_ + quantity.value()
                                           : -(Int128{position_} - open_sell_ - quantity.value());
    if (worst > limits_.max_position) {
        return RiskCheck::PositionLimit;
    }

    ++open_orders_;
    (side == Side::Buy ? open_buy_ : open_sell_) += quantity.value();
    return RiskCheck::Accepted;
}

void Portfolio::on_report(const ExecutionReport& report) noexcept
{
    switch (report.type) {
    case ReportType::Fill:
        apply_fill(report);
        release(report.side, report.quantity, report.leaves.is_zero());
        return;
    case ReportType::Cancelled:
    case ReportType::Expired:
    case ReportType::Rejected:
        release(report.side, report.quantity, true);
        return;
    case ReportType::Accepted:
    case ReportType::CancelRejected:
        return;
    }
}

void Portfolio::release(Side side, Quantity quantity, bool order_done) noexcept
{
    std::uint64_t& open = side == Side::Buy ? open_buy_ : open_sell_;
    assert(quantity.value() <= open && (!order_done || open_orders_ > 0));

    open -= quantity.value();
    open_orders_ -= order_done ? 1 : 0;
}

void Portfolio::apply_fill(const ExecutionReport& report) noexcept
{
    const auto quantity = static_cast<std::int64_t>(report.quantity.value());
    const std::int64_t fill = report.side == Side::Buy ? quantity : -quantity;
    const std::int64_t fee = report.liquidity == Liquidity::Maker ? fee_schedule_.maker_fee_per_share
        : report.liquidity == Liquidity::Taker                    ? fee_schedule_.taker_fee_per_share
                                                                  : 0;

    cash_ = to_money(Int128{cash_} - Int128{fill} * report.price.ticks);
    fees_ = to_money(Int128{fees_} + Int128{fee} * quantity);

    // Average cost: a fill against the position closes up to |position| at
    // its average cost (truncated toward zero, so realised + unrealised
    // stays exact); any excess opens the other side at the fill price.
    std::int64_t opening = fill;
    if (position_ != 0 && (position_ > 0) != (fill > 0)) {
        const std::int64_t open = std::abs(position_);
        const std::int64_t closing = std::min(quantity, open);
        cost_ -= to_money(Int128{cost_} * closing / open);
        opening = fill > 0 ? fill - closing : fill + closing;
    }
    cost_ = to_money(Int128{cost_} + Int128{opening} * report.price.ticks);

    position_ += fill;
    max_abs_position_ = std::max(max_abs_position_, std::abs(position_));
}

void Portfolio::mark(const MarketBook& book) noexcept
{
    LevelInfo level;
    if (book.best(Side::Buy, level)) {
        bid_ = level.price;
    }
    if (book.best(Side::Sell, level)) {
        ask_ = level.price;
    }

    const std::int64_t now = pnl();
    peak_pnl_ = std::max(peak_pnl_, now);
    max_drawdown_ = std::max(max_drawdown_, peak_pnl_ - now);
}

std::int64_t Portfolio::realised_pnl() const noexcept
{
    return to_money(Int128{cash_} + cost_);
}

std::int64_t Portfolio::unrealised_pnl() const noexcept
{
    const Price mark = position_ > 0 ? bid_ : ask_;
    return to_money(Int128{position_} * mark.ticks - cost_);
}

std::int64_t Portfolio::pnl() const noexcept
{
    const Price mark = position_ > 0 ? bid_ : ask_;
    return to_money(Int128{cash_} + Int128{position_} * mark.ticks - fees_);
}

} // namespace simulator
