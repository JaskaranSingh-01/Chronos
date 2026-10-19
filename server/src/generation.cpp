#include "chronos/server/generation.hpp"

#include <utility>

#include "simulator/market/market_book.hpp"

namespace simulator::server {
namespace {

ErrorCode risk_error(RiskCheck check) noexcept
{
    switch (check) {
    case RiskCheck::Accepted:
        return ErrorCode::None;
    case RiskCheck::InvalidOrder:
        return ErrorCode::RiskInvalidOrder;
    case RiskCheck::OrderTooLarge:
        return ErrorCode::RiskOrderTooLarge;
    case RiskCheck::PositionLimit:
        return ErrorCode::RiskPositionLimit;
    case RiskCheck::TooManyOpenOrders:
        return ErrorCode::RiskTooManyOpenOrders;
    }
    return ErrorCode::BadRequest;
}

// 00:00 New York time on the first day; 0 without days.
Timestamp first_midnight(const DayList& days) noexcept
{
    return days.count == 0 ? 0 : session_midnight_ns(days.day[0], us_eastern_utc_offset(days.day[0]));
}

// Cancels every resting order, so a strategy swap leaves none holding a reservation.
// ponytail: orders still in flight rest; cancel them manually after detach.
void cancel_resting(const Generation& generation, OrderGateway& gateway) noexcept
{
    for (const ShadowOrder& order : generation.execution.resting_orders()) {
        gateway.cancel(order.id);  // only queues an event, so the span stays valid
    }
}

}  // namespace

Generation::Generation(const SessionConfig& config, const RunSpec& run, std::unique_ptr<MarketEventSource> market)
    : spec(run),
      source(std::move(market)),
      book(config.symbol, config.book_capacity),
      execution(first_midnight(run.days)),
      portfolio(config.instrument, run.limits, run.fees),
      strategy(run.strategy),
      loop(*source, book, execution, portfolio, strategy, run.latency,
           {.check_invariants_every = config.check_invariants_every, .session_midnight = first_midnight(run.days)})
{}

Digests digests(const Generation& generation) noexcept
{
    Digests d{.book = generation.book.digest(), .execution = fnv1a_offset_basis, .portfolio = fnv1a_offset_basis};
    const ExecutionStatistics& e = generation.execution.statistics();
    for (const std::uint64_t value :
         {e.maker_fills, e.maker_quantity, e.taker_fills, e.taker_quantity, e.accepted_quantity, e.would_cross_rejects,
          e.not_trading_rejects, e.cancelled, e.cancel_rejects, e.fifo_anomalies}) {
        fnv1a_mix(d.execution, value);
    }
    for (const ShadowOrder& order : generation.execution.resting_orders()) {
        for (const std::uint64_t value : {order.id, static_cast<std::uint64_t>(order.price.ticks), order.remaining,
                                          order.ahead_quantity, std::uint64_t{order.side == Side::Buy}}) {
            fnv1a_mix(d.execution, value);
        }
    }
    const Portfolio& p = generation.portfolio;
    for (const std::int64_t value : {p.position(), p.cash(), p.realised_pnl(), p.unrealised_pnl(), p.fees(),
                                     p.max_drawdown(), static_cast<std::int64_t>(p.open_orders())}) {
        fnv1a_mix(d.portfolio, static_cast<std::uint64_t>(value));
    }
    return d;
}

Applied apply_command(Generation& generation, const Command& command) noexcept
{
    const bool manual = command.type == CommandType::Order || command.type == CommandType::Cancel;
    if (manual && generation.strategy.attached()) {
        return {.error = ErrorCode::NotReady};
    }
    OrderGateway& gateway = generation.loop.gateway();
    switch (command.type) {
    case CommandType::Order: {
        Price price = command.price;
        if (command.order_type == OrderType::Market) {
            LevelInfo best;
            if (!generation.book.best(command.side == Side::Buy ? Side::Sell : Side::Buy, best)) {
                return {.error = ErrorCode::NoLiquidity};
            }
            const std::int64_t band = best.price.ticks * market_band_percent / 100;
            price = Price{command.side == Side::Buy ? best.price.ticks + band : best.price.ticks - band};
        }
        const TimeInForce tif = command.order_type == OrderType::PostOnly ? TimeInForce::Day : TimeInForce::Ioc;
        Applied applied;
        applied.error = risk_error(gateway.send(command.side, price, command.quantity, tif, applied.order_id));
        return applied;
    }
    case CommandType::Cancel:
        gateway.cancel(command.order_id);
        return {.order_id = command.order_id};
    case CommandType::Attach:
        cancel_resting(generation, gateway);
        generation.strategy.attach(command.strategy);
        return {};
    case CommandType::Detach:
        cancel_resting(generation, gateway);
        generation.strategy.attach(StrategySpec{});
        return {};
    }
    return {.error = ErrorCode::BadRequest};
}

}  // namespace simulator::server
