// The example plugin of docs/strategies.md: the whole strategy, its params and its registration in one file, with no
// change anywhere else. It buys after a run of rising trades and sells what it holds after a fixed time.
#include "simulator/engine/strategy_registry.hpp"

namespace {

using namespace simulator;

// Each field's default is the param's default.
struct TrendParams
{
    std::uint64_t clip{100};           // shares per entry
    std::int64_t run_length{3};        // rising trades in a row that trigger an entry
    Timestamp hold_ns{1'000'000'000};  // how long a position is held before it is sold
};

class TrendTaker
{
private:
    static constexpr std::uint64_t exit_timer = 1;

    TrendParams params_;
    Price last_trade_{};
    std::int64_t rising_{0};  // trades in a row above the one before
    bool holding_{false};     // from an entry until the position is flat again
    std::uint64_t entries_{0};
    std::uint64_t exits_{0};

public:
    explicit TrendTaker(const TrendParams& params) noexcept
        : params_(params)
    {}

    // Called after every event the book applied; ctx.trade is set when the event printed a trade.
    void on_market(StrategyContext& ctx, const MarketEvent&) noexcept
    {
        if (ctx.trade == nullptr) {
            return;
        }
        rising_ = last_trade_.ticks != 0 && ctx.trade->price > last_trade_ ? rising_ + 1 : 0;
        last_trade_ = ctx.trade->price;
        LevelInfo ask;
        if (holding_ || rising_ < params_.run_length || ctx.book.trading_state() != TradingState::Trading ||
            !ctx.book.best(Side::Sell, ask)) {
            return;
        }
        OrderId id{0};
        if (ctx.gateway.send(Side::Buy, ask.price, Quantity{params_.clip}, TimeInForce::Ioc, id) ==
            RiskCheck::Accepted) {
            holding_ = true;
            ++entries_;
            ctx.set_timer(ctx.now() + params_.hold_ns, exit_timer);
        }
    }

    // Reports arrive after the report latency, once the portfolio has booked them.
    void on_report(StrategyContext&, const ExecutionReport&) noexcept {}

    // Sells what the entry bought; a sell that fills in part is sent again a hold later, until the position is flat.
    void on_timer(StrategyContext& ctx, std::uint64_t) noexcept
    {
        const std::int64_t position = ctx.portfolio.position();
        LevelInfo bid;
        if (position <= 0) {
            holding_ = false;
            return;
        }
        if (ctx.book.best(Side::Buy, bid)) {
            OrderId id{0};
            if (ctx.gateway.send(Side::Sell, bid.price, Quantity{static_cast<std::uint64_t>(position)},
                                 TimeInForce::Ioc, id) == RiskCheck::Accepted) {
                ++exits_;
            }
        }
        ctx.set_timer(ctx.now() + params_.hold_ns, exit_timer);
    }

    std::uint64_t entries() const noexcept
    {
        return entries_;
    }
    std::uint64_t exits() const noexcept
    {
        return exits_;
    }
};

[[maybe_unused]] const bool registered = register_plugin(StrategyPlugin<TrendTaker, TrendParams>{
    .name = "trend_taker",
    .label = "Trend taker (example)",
    .summary = "The plugin guide's example: an IOC buy after a run of rising trades, sold after a hold",
    .params = {param("clip", &TrendParams::clip, 1, 1'000'000, "shares per entry"),
               param("run_length", &TrendParams::run_length, 1, 1'000, "rising trades in a row that trigger an entry"),
               param("hold_ns", &TrendParams::hold_ns, 1, 3'600'000'000'000, "how long a position is held, in ns")},
    // One entry or exit in flight, and never more than one clip held.
    .limits =
        [](const TrendParams& p) {
            return RiskLimits{.max_position = static_cast<std::int64_t>(p.clip),
                              .max_order_quantity = Quantity{p.clip},
                              .max_open_orders = 1};
        },
    .stats =
        [](const TrendTaker& s, std::vector<Stat>& out) {
            out = {{"Entries", static_cast<double>(s.entries())}, {"Exits sent", static_cast<double>(s.exits())}};
        },
});

}  // namespace
