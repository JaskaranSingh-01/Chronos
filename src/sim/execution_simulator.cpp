#include "simulator/sim/execution_simulator.hpp"

#include <algorithm>
#include <cstdint>

namespace simulator {

void ExecutionSimulator::on_order_arrive(const OrderRequest& request, Timestamp now, const MarketBook& book,
                                         std::vector<ExecutionReport>& out) noexcept
{
    // Every order ends with exactly one leaves-0 report, so the Portfolio
    // always releases its reservation.
    RejectReason reason = RejectReason::None;
    if (request.quantity.is_zero()) {
        reason = RejectReason::InvalidOrder;
    } else if (request.time_in_force != TimeInForce::Ioc) {
        reason = RejectReason::UnsupportedTimeInForce;
    }
    if (reason != RejectReason::None) {
        out.push_back(ExecutionReport{.exchange_time = now, .order_id = request.id, .quantity = request.quantity,
                                      .type = ReportType::Rejected, .reason = reason, .side = request.side});
        return;
    }

    const bool buy = request.side == Side::Buy;
    std::uint64_t remaining = request.quantity.value();

    book.visit_levels(buy ? Side::Sell : Side::Buy, [&](const LevelInfo& level) {
        if (buy ? level.price > request.price : level.price < request.price) {
            return false;
        }
        const std::uint64_t filled = std::min(remaining, level.quantity);
        remaining -= filled;
        out.push_back(ExecutionReport{.exchange_time = now, .order_id = request.id, .price = level.price,
                                      .quantity = Quantity{filled}, .leaves = Quantity{remaining},
                                      .type = ReportType::Fill, .liquidity = Liquidity::Taker,
                                      .side = request.side});
        return remaining > 0;
    });

    if (remaining > 0) {
        out.push_back(ExecutionReport{.exchange_time = now, .order_id = request.id,
                                      .quantity = Quantity{remaining}, .type = ReportType::Expired,
                                      .side = request.side});
    }
}

} // namespace simulator
