#include "simulator/sim/execution_simulator.hpp"

#include <algorithm>
#include <cstdint>

namespace simulator {
namespace {

// a has priority over b on `side`: a higher bid or a lower ask.
bool better(Side side, Price a, Price b) noexcept
{
    return side == Side::Buy ? a > b : a < b;
}

} // namespace

const ShadowOrder* ExecutionSimulator::find(OrderId id) const noexcept
{
    const auto it = std::find_if(orders_.begin(), orders_.end(), [id](const ShadowOrder& o) { return o.id == id; });
    return it == orders_.end() ? nullptr : &*it;
}

void ExecutionSimulator::fill(ShadowOrder& order, std::uint64_t quantity, Timestamp now, FillCause cause,
                              std::vector<ExecutionReport>& out) noexcept
{
    order.remaining -= quantity;
    ++statistics_.maker_fills;
    ++statistics_
          .fills_by[static_cast<std::size_t>(cause)][static_cast<std::size_t>(session_phase(now, session_midnight_))];
    statistics_.maker_quantity += quantity;
    out.push_back(ExecutionReport{.exchange_time = now,
                                  .order_id = order.id,
                                  .price = order.price,
                                  .quantity = Quantity{quantity},
                                  .leaves = Quantity{order.remaining},
                                  .type = ReportType::Fill,
                                  .liquidity = Liquidity::Maker,
                                  .side = order.side,
                                  .cause = cause,
                                  .entry_ahead_quantity = order.entry_ahead_quantity});
}

void ExecutionSimulator::cross(Side side, Price price, std::uint64_t quantity, Timestamp now, FillCause cause,
                               std::vector<ExecutionReport>& out) noexcept
{
    for (ShadowOrder& o : orders_) {
        if (quantity == 0) {
            return;
        }
        // Equal counts: a sell at our bid, or a buy at our ask, would have hit us.
        if (o.side != side && (o.side == Side::Buy ? price <= o.price : price >= o.price)) {
            const std::uint64_t filled = std::min(o.remaining, quantity);
            quantity -= filled;
            fill(o, filled, now, cause, out);
        }
    }
}

void ExecutionSimulator::on_market(const MarketEvent& event, const MarketBook& book,
                                   std::vector<ExecutionReport>& out) noexcept
{
    if (orders_.empty() || event.symbol != book.symbol()) {
        return;
    }
    const std::size_t before = out.size();
    // Pre-event state. While not Trading nothing matches, so no historical
    // event fills ours (a halted book may cross); rule (b) still runs.
    const bool trading = book.trading_state() == TradingState::Trading;

    if (event.type == MarketEventType::Add) {
        if (trading && book.find(event.order_id) == nullptr) {  // else not trading, or a duplicate the book rejects
            cross(event.side, event.price, event.quantity.value(), event.timestamp, FillCause::RuleCAdd, out);
        }
    } else if (event.type == MarketEventType::Execute || event.type == MarketEventType::Cancel ||
               event.type == MarketEventType::Delete || event.type == MarketEventType::Replace) {
        // Trade and Status touch no visible order; Modify is not applied by MarketBook yet.
        const Order* h = book.find(event.order_id);
        if (h == nullptr) {
            return;
        }
        const bool reduce = event.type == MarketEventType::Execute || event.type == MarketEventType::Cancel;
        const bool replace = event.type == MarketEventType::Replace;
        if ((reduce && (event.quantity.is_zero() || event.quantity > h->remaining_quantity())) ||
            (replace &&
             (event.new_order_id == event.order_id || book.find(event.new_order_id) != nullptr ||
              event.quantity.is_zero()))) {
            return;
        }

        const Side side = h->side();
        const std::uint64_t removed = reduce ? event.quantity.value() : h->remaining_quantity().value();
        // Can fill us: (a), (d).
        std::uint64_t left = trading && event.type == MarketEventType::Execute ? removed : 0;
        // ITCH C carries the trade's own price, which can beat h's displayed
        // one: a price-slid order ranks at a better, non-displayed price.
        const bool with_price = left != 0 && event.price.ticks != 0;
        const Price traded = with_price ? event.price : h->price();

        for (ShadowOrder& o : orders_) {
            if (o.side != side || better(side, h->price(), o.price)) {
                continue;  // other side, or h at a better price than ours
            }
            if (o.price == h->price() && h->sequence() <= o.anchor_sequence) {
                o.ahead_quantity -= std::min(o.ahead_quantity, removed);  // (b)
            } else if (left != 0 && !better(side, traded, o.price)) {  // h behind us (a), or worse (d)
                const std::uint64_t filled = std::min(o.remaining, left);
                left -= filled;
                const FillCause cause = o.price == h->price()
                    ? (with_price ? FillCause::RuleAExecuteWithPrice : FillCause::RuleAExecute)
                    : (with_price ? FillCause::RuleDExecuteWithPrice : FillCause::RuleDExecute);
                if (o.ahead_quantity != 0) {
                    ++statistics_.fifo_anomalies;
                    ++statistics_.anomalies_by[static_cast<std::size_t>(cause)][static_cast<std::size_t>(
                        session_phase(event.timestamp, session_midnight_))];
                    if (anomalies_.size() < max_fifo_anomaly_samples) {
                        anomalies_.push_back(FifoAnomaly{.event = event,
                                                         .h_price = h->price(),
                                                         .h_side = side,
                                                         .h_sequence = h->sequence(),
                                                         .h_remaining = h->remaining_quantity().value(),
                                                         .order = o,
                                                         .level_quantity = book.quantity_at(o.side, o.price)});
                    }
                }
                fill(o, filled, event.timestamp, cause, out);
            }
        }
        if (replace && trading) {
            cross(side, event.price, event.quantity.value(), event.timestamp, FillCause::RuleCReplace, out);
        }
    } else if (event.type == MarketEventType::Clear) {  // a new day: each resting order expires, as DAY orders do
        // ponytail: an order in flight at a Clear rests on the new day; drop it by send time if needed.
        for (const ShadowOrder& o : orders_) {
            out.push_back(ExecutionReport{.exchange_time = event.timestamp,
                                          .order_id = o.id,
                                          .quantity = Quantity{o.remaining},
                                          .type = ReportType::Expired,
                                          .side = o.side});
        }
        orders_.clear();
    }

    if (out.size() != before) {
        std::erase_if(orders_, [](const ShadowOrder& o) { return o.remaining == 0; });
    }
}

void ExecutionSimulator::on_order_arrive(const OrderRequest& request, Timestamp now, const MarketBook& book,
                                         std::vector<ExecutionReport>& out) noexcept
{
    const bool buy = request.side == Side::Buy;
    const Side opposite = buy ? Side::Sell : Side::Buy;

    // Every order ends with exactly one leaves-0 report, or rests, so the
    // Portfolio always releases its reservation in the end.
    RejectReason reason = RejectReason::None;
    LevelInfo best;
    if (request.quantity.is_zero()) {
        reason = RejectReason::InvalidOrder;
    } else if (book.trading_state() != TradingState::Trading) {
        reason = RejectReason::NotTrading;
        ++statistics_.not_trading_rejects;
    } else if (request.time_in_force == TimeInForce::Day && book.best(opposite, best) &&
               (buy ? request.price >= best.price : request.price <= best.price)) {
        reason = RejectReason::WouldCross;
        ++statistics_.would_cross_rejects;
    }
    if (reason != RejectReason::None) {
        out.push_back(ExecutionReport{.exchange_time = now,
                                      .order_id = request.id,
                                      .quantity = request.quantity,
                                      .type = ReportType::Rejected,
                                      .reason = reason,
                                      .side = request.side});
        return;
    }

    if (request.time_in_force == TimeInForce::Day) {
        const std::uint64_t ahead = book.quantity_at(request.side, request.price);
        const ShadowOrder order{.id = request.id,
                                .side = request.side,
                                .price = request.price,
                                .remaining = request.quantity.value(),
                                .anchor_sequence = book.last_sequence(),
                                .ahead_quantity = ahead,
                                .entry_ahead_quantity = ahead};
        // Behind every order of ours on this side at a price at least as good.
        orders_.insert(std::find_if(orders_.begin(), orders_.end(),
                                    [&order](const ShadowOrder& o) {
                                        return o.side == order.side && better(order.side, order.price, o.price);
                                    }),
                       order);
        statistics_.accepted_quantity += order.remaining;
        out.push_back(ExecutionReport{.exchange_time = now,
                                      .order_id = request.id,
                                      .quantity = request.quantity,
                                      .leaves = request.quantity,
                                      .type = ReportType::Accepted,
                                      .side = request.side});
        return;
    }

    std::uint64_t remaining = request.quantity.value();

    book.visit_levels(opposite, [&](const LevelInfo& level) {
        if (buy ? level.price > request.price : level.price < request.price) {
            return false;
        }
        const std::uint64_t filled = std::min(remaining, level.quantity);
        remaining -= filled;
        ++statistics_.taker_fills;
        statistics_.taker_quantity += filled;
        out.push_back(ExecutionReport{.exchange_time = now,
                                      .order_id = request.id,
                                      .price = level.price,
                                      .quantity = Quantity{filled},
                                      .leaves = Quantity{remaining},
                                      .type = ReportType::Fill,
                                      .liquidity = Liquidity::Taker,
                                      .side = request.side});
        return remaining > 0;
    });

    if (remaining > 0) {
        out.push_back(ExecutionReport{.exchange_time = now,
                                      .order_id = request.id,
                                      .quantity = Quantity{remaining},
                                      .type = ReportType::Expired,
                                      .side = request.side});
    }
}

void ExecutionSimulator::on_cancel_arrive(OrderId id, Timestamp now, std::vector<ExecutionReport>& out) noexcept
{
    const auto it = std::find_if(orders_.begin(), orders_.end(), [id](const ShadowOrder& o) { return o.id == id; });
    if (it == orders_.end()) {
        ++statistics_.cancel_rejects;
        out.push_back(ExecutionReport{.exchange_time = now, .order_id = id, .type = ReportType::CancelRejected});
        return;
    }
    ++statistics_.cancelled;
    out.push_back(ExecutionReport{.exchange_time = now,
                                  .order_id = id,
                                  .quantity = Quantity{it->remaining},
                                  .type = ReportType::Cancelled,
                                  .side = it->side});
    orders_.erase(it);
}

} // namespace simulator
