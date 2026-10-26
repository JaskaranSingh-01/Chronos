#include "chronos/server/generation.hpp"

#include <utility>
#include <vector>

#include "chronos/server/itch_input.hpp"
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
    case RiskCheck::Closed:  // only an attached strategy's window closes the gateway
        return ErrorCode::NotReady;
    }
    return ErrorCode::BadRequest;
}

// 00:00 New York time on the first day; 0 without days.
Timestamp first_midnight(const DayList& days) noexcept
{
    return days.count == 0 ? 0 : session_midnight_ns(days.day[0], us_eastern_utc_offset(days.day[0]));
}

// Each day's feed session from a day chain; any other source's days take the 09:30-16:00 fallback.
std::vector<FeedSession> sessions(const DayList& days, const MarketEventSource& source)
{
    const auto* chain = dynamic_cast<const DaySequenceSource*>(&source);
    std::vector<FeedSession> found;
    for (std::size_t i = 0; i < days.count; ++i) {
        const Timestamp midnight = session_midnight_ns(days.day[i], us_eastern_utc_offset(days.day[i]));
        found.push_back(chain != nullptr && i < chain->days().size()
                            ? chain->days()[i]->session
                            : FeedSession{.open = midnight + RTH_OPEN_NS, .close = midnight + RTH_CLOSE_NS});
    }
    return found;
}

}  // namespace

Generation::Generation(const SessionConfig& config, const RunSpec& run, std::unique_ptr<MarketEventSource> market)
    : spec(run),
      source(std::move(market)),
      book(config.symbol, config.book_capacity, config.book),
      execution(first_midnight(run.days)),
      portfolio(config.instrument, run.limits, run.fees),
      strategy(run.strategy, execution, run.window, sessions(run.days, *source)),
      loop(*source, book, execution, portfolio, strategy, run.latency,
           {.check_invariants_every = config.check_invariants_every, .session_midnight = first_midnight(run.days)})
{}

void finish(Generation& generation) noexcept
{
    const std::vector<FeedSession>& days = generation.strategy.sessions();
    if (generation.spec.window == TradingWindow::Regular && !days.empty()) {
        // The end-of-stream drain runs the close timer too; this pins the final clock at the close.
        generation.loop.advance_to(days.back().close);
    }
    generation.loop.finish();
}

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
    // A strategy swap cancels every resting order, so none holds a reservation.
    // ponytail: orders still in flight rest; cancel them manually after detach.
    case CommandType::Attach:
        cancel_resting(generation.execution, gateway);
        generation.strategy.attach(command.strategy);
        gateway.set_open(generation.strategy.gateway_open());
        return {};
    case CommandType::Detach:
        cancel_resting(generation.execution, gateway);
        generation.strategy.attach(StrategySpec{});
        gateway.set_open(generation.strategy.gateway_open());
        return {};
    }
    return {.error = ErrorCode::BadRequest};
}

}  // namespace simulator::server
