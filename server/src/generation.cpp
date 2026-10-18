#include "chronos/server/generation.hpp"

#include <utility>

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

// Cancels every resting order, so a strategy swap leaves none holding a reservation.
// ponytail: orders still in flight rest; cancel them manually after detach.
void cancel_resting(const Generation& generation, OrderGateway& gateway) noexcept
{
    for (const ShadowOrder& order : generation.execution.resting_orders()) {
        gateway.cancel(order.id);  // only queues an event, so the span stays valid
    }
}

}  // namespace

Generation::Generation(const SessionConfig& config, std::unique_ptr<MarketEventSource> market, const StrategySpec& spec)
    : source(std::move(market)),
      book(config.symbol, config.book_capacity),
      execution(config.options.session_midnight),
      portfolio(config.instrument, config.limits, config.fees),
      strategy(spec),
      loop(*source, book, execution, portfolio, strategy, config.latency, config.options)
{}

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
