#pragma once

#include <cstdint>

#include "simulator/engine/strategy.hpp"

namespace simulator {

struct ImbalanceTakerParams
{
    double threshold{3.0};               // >= 1: one side's top size over the other's
    std::uint64_t clip{100};             // shares per order
    std::int64_t max_position{500};      // |position| the strategy itself will not exceed
    Timestamp cooldown_ns{100'000'000};  // after each send attempt, accepted or not
};

struct ImbalanceTakerStats
{
    std::uint64_t sent{0};          // accepted by the gateway's risk check
    std::uint64_t risk_rejects{0};  // refused by it
    std::uint64_t fills{0};         // Fill reports
    std::uint64_t expired{0};       // Expired reports (IOC leftovers)
    std::uint64_t bought{0};        // shares
    std::uint64_t sold{0};

    bool operator==(const ImbalanceTakerStats&) const = default;
};

// A TOY that exercises the backtest path end to end; it is not a trading idea
// and its PnL means nothing. While the book is Trading: when the best bid's
// size is at least threshold x the best ask's, it sends a buy IOC for `clip`
// at the best ask; in the mirror case a sell IOC at the best bid. It ignores
// fees, pays the spread on every trade, has no fair value, and leans on the
// no-impact fill model (spec §11 Q2). MarketMaker is its passive counterpart.
class ImbalanceTaker
{
private:
    ImbalanceTakerParams params_;
    ImbalanceTakerStats stats_;
    Timestamp ready_at_{0};  // end of the cooldown

    void send(StrategyContext& ctx, Side side, Price price) noexcept
    {
        ready_at_ = ctx.now() + params_.cooldown_ns;
        OrderId id{0};
        if (ctx.gateway.send(side, price, Quantity{params_.clip}, TimeInForce::Ioc, id) == RiskCheck::Accepted) {
            ++stats_.sent;
        } else {
            ++stats_.risk_rejects;
        }
    }

public:
    explicit ImbalanceTaker(ImbalanceTakerParams params) noexcept
        : params_(params) {}

    void on_market(StrategyContext& ctx, const MarketEvent&) noexcept
    {
        LevelInfo bid;
        LevelInfo ask;
        if (ctx.book.trading_state() != TradingState::Trading || ctx.now() < ready_at_ ||
            !ctx.book.best(Side::Buy, bid) || !ctx.book.best(Side::Sell, ask)) {
            return;
        }

        const std::int64_t position = ctx.portfolio.position();
        const auto clip = static_cast<std::int64_t>(params_.clip);
        const auto bid_size = static_cast<double>(bid.quantity);
        const auto ask_size = static_cast<double>(ask.quantity);

        if (bid_size >= params_.threshold * ask_size && position + clip <= params_.max_position) {
            send(ctx, Side::Buy, ask.price);
        } else if (ask_size >= params_.threshold * bid_size && position - clip >= -params_.max_position) {
            send(ctx, Side::Sell, bid.price);
        }
    }

    void on_report(StrategyContext&, const ExecutionReport& report) noexcept
    {
        if (report.type == ReportType::Fill) {
            ++stats_.fills;
            (report.side == Side::Buy ? stats_.bought : stats_.sold) += report.quantity.value();
        } else if (report.type == ReportType::Expired) {
            ++stats_.expired;
        }
    }

    void on_timer(StrategyContext&, std::uint64_t) noexcept {}

    const ImbalanceTakerStats& stats() const noexcept { return stats_; }
};

} // namespace simulator
