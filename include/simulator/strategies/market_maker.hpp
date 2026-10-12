#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "simulator/engine/strategy.hpp"

namespace simulator {

struct MarketMakerParams
{
    std::uint64_t quote_size{100};                // shares per quote
    std::int64_t max_position{500};               // |position| a fill of the next quote may not exceed
    Timestamp requote_cooldown_ns{100'000'000};   // a quote lives at least this long before a requote
    std::int64_t improve_ticks{0};                // 0 = join the best bid and ask; N = N price ticks
                                                  // inside them while the spread is wider than 2N
};

struct MarketMakerStats
{
    std::uint64_t quotes{0};        // Day orders accepted by the gateway's risk check
    std::uint64_t risk_rejects{0};  // refused by it
    std::uint64_t cancels{0};       // cancels sent
    std::uint64_t fills{0};         // Fill reports
    std::uint64_t rejects{0};       // Rejected reports (WouldCross)
    std::uint64_t bought{0};        // shares
    std::uint64_t sold{0};

    bool operator==(const MarketMakerStats&) const = default;
};

// A TOY that exercises the passive path (resting orders, queue position,
// cancels) end to end; it is not a trading idea and its PnL means nothing. It
// has no fair value, no skew and no adverse-selection defence.
//
// It keeps at most one post-only Day quote per side, at the best bid and ask
// (or improve_ticks inside), whenever the book is two-sided and not locked or
// crossed. Inventory: it does not quote a side whose next full fill would
// take |position| past max_position, and cancels such a quote at once.
//
// Requote = cancel, then replace on the ack: when a side's desired price
// differs from its live quote's and the quote is at least requote_cooldown_ns
// old, it cancels; the Cancelled report frees the side and the new quote goes
// out in that same callback. So a side never has two orders in flight and
// max_open_orders = 2 always suffices, at the cost of one round trip without
// a quote. A WouldCross reject backs that side off for the cooldown.
class MarketMaker
{
private:
    struct Quote
    {
        OrderId id{0};          // 0 = no order on this side
        Price price{};
        bool cancelling{false};
        Timestamp ready_at{0};  // no new quote or requote before this
    };

    MarketMakerParams params_;
    MarketMakerStats stats_;
    std::array<Quote, 2> quotes_{};  // index = Side

    static std::size_t index(Side side) noexcept { return static_cast<std::size_t>(side); }

    // False if `side` should have no quote now.
    bool target(const StrategyContext& ctx, Side side, Price& price) const noexcept
    {
        LevelInfo bid;
        LevelInfo ask;
        if (!ctx.book.best(Side::Buy, bid) || !ctx.book.best(Side::Sell, ask) || bid.price >= ask.price) {
            return false;
        }
        const std::int64_t position = ctx.portfolio.position();
        const auto size = static_cast<std::int64_t>(params_.quote_size);
        if (side == Side::Buy ? position + size > params_.max_position : position - size < -params_.max_position) {
            return false;
        }
        const std::int64_t improve =
            ask.price.ticks - bid.price.ticks > 2 * params_.improve_ticks ? params_.improve_ticks : 0;
        price = side == Side::Buy ? Price{bid.price.ticks + improve} : Price{ask.price.ticks - improve};
        return true;
    }

    void update(StrategyContext& ctx, Side side) noexcept
    {
        Quote& quote = quotes_[index(side)];
        Price price;
        const bool want = target(ctx, side, price);
        if (quote.cancelling || (want && ctx.now() < quote.ready_at)) {
            return;  // a cancel in flight, or cooling down (dropping a side does not wait)
        }

        if (quote.id != 0) {
            if (!want || price != quote.price) {
                ctx.gateway.cancel(quote.id);
                quote.cancelling = true;
                ++stats_.cancels;
            }
            return;
        }
        if (!want) {
            return;
        }
        quote.ready_at = ctx.now() + params_.requote_cooldown_ns;
        OrderId id{0};
        if (ctx.gateway.send(side, price, Quantity{params_.quote_size}, TimeInForce::Day, id) == RiskCheck::Accepted) {
            quote.id = id;
            quote.price = price;
            ++stats_.quotes;
        } else {
            ++stats_.risk_rejects;
        }
    }

    void update(StrategyContext& ctx) noexcept
    {
        update(ctx, Side::Buy);
        update(ctx, Side::Sell);
    }

public:
    explicit MarketMaker(MarketMakerParams params) noexcept
        : params_(params) {}

    void on_market(StrategyContext& ctx, const MarketEvent&) noexcept { update(ctx); }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        if (report.type == ReportType::Fill) {
            ++stats_.fills;
            (report.side == Side::Buy ? stats_.bought : stats_.sold) += report.quantity.value();
        } else if (report.type == ReportType::Rejected) {
            ++stats_.rejects;
        }

        // By id: a CancelRejected names no side, and may come after its order is done.
        for (Quote& quote : quotes_) {
            if (quote.id != report.order_id) {
                continue;
            }
            const bool done = report.type == ReportType::Cancelled || report.type == ReportType::Rejected ||
                              report.type == ReportType::Expired ||
                              (report.type == ReportType::Fill && report.leaves.is_zero());
            if (done) {
                quote.id = 0;
                quote.cancelling = false;
            } else if (report.type == ReportType::CancelRejected) {
                quote.cancelling = false;
            }
            if (report.type == ReportType::Rejected) {
                quote.ready_at = ctx.now() + params_.requote_cooldown_ns;
            }
        }
        update(ctx);
    }

    void on_timer(StrategyContext&, std::uint64_t) noexcept {}

    const MarketMakerStats& stats() const noexcept { return stats_; }
};

} // namespace simulator
