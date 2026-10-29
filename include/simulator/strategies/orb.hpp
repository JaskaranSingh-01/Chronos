#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "simulator/engine/strategy.hpp"

namespace simulator {

struct OrbParams
{
    std::uint32_t range_minutes{15};  // the opening range: [open, open + range_minutes)
    std::uint64_t clip{100};          // shares per entry

    bool operator==(const OrbParams&) const = default;
};

// Decisions and shares counted apart: an accepted IOC is a decision whatever it fills.
struct OrbStats
{
    std::uint64_t long_entries{0};    // entry IOCs the gateway accepted
    std::uint64_t short_entries{0};
    std::uint64_t stops{0};           // days that began closing on a stop
    std::uint64_t flattens{0};        // days that began closing at the flatten deadline, or carried a position in
    std::uint64_t closing_orders{0};  // closing IOCs accepted, retries included
    std::uint64_t risk_rejects{0};
    std::uint64_t no_range_days{0};
    std::uint64_t requested{0};       // shares in accepted orders
    std::uint64_t bought{0};          // filled
    std::uint64_t sold{0};
    std::uint64_t expired{0};         // IOC remainders and rejected orders' shares
    bool has_range{false};            // the last day's range
    Price range_high{};
    Price range_low{};

    bool operator==(const OrbStats&) const = default;
};

// The opening range breakout (docs/backtesting.md). Each day: the high and low of trades
// in [open, open + range); from then on, the first trade strictly beyond either side enters `clip` shares by an IOC
// at the touch, once per direction; a trade strictly through the other side, or close - 5 min, closes the position
// by IOCs at the touch until it is flat. One order in flight at a time; nothing while the book is not trading.
class Orb
{
private:
    static constexpr Timestamp minute = 60'000'000'000;
    static constexpr Timestamp flatten_before_close = 5 * minute;

    OrbParams params_;
    std::span<const DaySession> sessions_;  // oldest first
    OrbStats stats_;
    std::size_t day_{0};
    bool started_{false};
    bool range_closed_{false};
    bool used_long_{false};
    bool used_short_{false};
    bool closing_{false};
    OrderId in_flight_{0};

    const DaySession& session() const noexcept
    {
        return sessions_[day_];
    }

    Timestamp range_end() const noexcept
    {
        return session().open + params_.range_minutes * minute;
    }

    Timestamp flatten_at() const noexcept
    {
        return session().close - flatten_before_close;
    }

    // A new day: its range and both directions reset. A position open at a day's start, one it did not open or one
    // carried in, is closed before anything else.
    void start_day(StrategyContext& ctx) noexcept
    {
        stats_.has_range = false;
        stats_.range_high = stats_.range_low = Price{};
        range_closed_ = false;
        used_long_ = used_short_ = false;
        if (ctx.portfolio.position() != 0 && !closing_) {
            closing_ = true;
            ++stats_.flattens;
        }
        ctx.set_timer(flatten_at(), day_);
    }

    void sync_day(StrategyContext& ctx) noexcept
    {
        if (!started_) {
            started_ = true;
            start_day(ctx);
        }
        while (day_ + 1 < sessions_.size() && ctx.now() >= sessions_[day_ + 1].open) {
            stats_.no_range_days += !range_closed_ && !stats_.has_range ? 1 : 0;  // its range end never came
            ++day_;
            start_day(ctx);
        }
    }

    // An IOC of `quantity` at the touch it crosses; false if it is not sent.
    bool send(StrategyContext& ctx, Side side, std::uint64_t quantity) noexcept
    {
        LevelInfo touch;
        if (in_flight_ != 0 || ctx.book.trading_state() != TradingState::Trading ||
            !ctx.book.best(side == Side::Buy ? Side::Sell : Side::Buy, touch)) {
            return false;
        }
        OrderId id{0};
        if (ctx.gateway.send(side, touch.price, Quantity{quantity}, TimeInForce::Ioc, id) != RiskCheck::Accepted) {
            ++stats_.risk_rejects;
            return false;
        }
        in_flight_ = id;
        stats_.requested += quantity;
        return true;
    }

    void begin_closing(std::uint64_t& reason) noexcept
    {
        closing_ = true;
        ++reason;
    }

    // Closing: IOCs of at most the clip, the order size the risk limits allow, until flat with nothing in flight.
    void close(StrategyContext& ctx) noexcept
    {
        const std::int64_t position = ctx.portfolio.position();
        if (position == 0) {
            closing_ = in_flight_ != 0;
            return;
        }
        const std::uint64_t remaining = static_cast<std::uint64_t>(std::llabs(position));
        if (send(ctx, position > 0 ? Side::Sell : Side::Buy, std::min(remaining, params_.clip))) {
            ++stats_.closing_orders;
        }
    }

    void enter(StrategyContext& ctx, Side side) noexcept
    {
        if (send(ctx, side, params_.clip)) {
            (side == Side::Buy ? used_long_ : used_short_) = true;
            ++(side == Side::Buy ? stats_.long_entries : stats_.short_entries);
        }
    }

    void on_trade(StrategyContext& ctx, const Trade& trade) noexcept
    {
        const Timestamp at = trade.timestamp;
        if (at < session().open) {
            return;
        }
        if (at < range_end()) {
            stats_.range_high = stats_.has_range ? std::max(stats_.range_high, trade.price) : trade.price;
            stats_.range_low = stats_.has_range ? std::min(stats_.range_low, trade.price) : trade.price;
            stats_.has_range = true;
            return;
        }
        if (!stats_.has_range || closing_ || at >= flatten_at()) {
            return;
        }
        const std::int64_t position = ctx.portfolio.position();
        if (position > 0 && trade.price < stats_.range_low) {
            begin_closing(stats_.stops);
        } else if (position < 0 && trade.price > stats_.range_high) {
            begin_closing(stats_.stops);
        } else if (position == 0 && !used_long_ && trade.price > stats_.range_high) {
            enter(ctx, Side::Buy);
        } else if (position == 0 && !used_short_ && trade.price < stats_.range_low) {
            enter(ctx, Side::Sell);
        }
    }

    // After the range: a day without one is counted; after the flatten deadline an open position begins closing.
    void on_clock(StrategyContext& ctx) noexcept
    {
        if (!range_closed_ && ctx.now() >= range_end()) {
            range_closed_ = true;
            stats_.no_range_days += stats_.has_range ? 0 : 1;
        }
        if (!closing_ && ctx.now() >= flatten_at() && ctx.portfolio.position() != 0) {
            begin_closing(stats_.flattens);
        }
    }

public:
    // `sessions`: each day's, oldest first; the caller keeps them alive. With none, it never trades.
    Orb(OrbParams params, std::span<const DaySession> sessions) noexcept
        : params_(params), sessions_(sessions)
    {}

    void on_market(StrategyContext& ctx, const MarketEvent&) noexcept
    {
        if (sessions_.empty()) {
            return;  // no session, no range: a stream of no day never trades
        }
        sync_day(ctx);
        on_clock(ctx);
        if (ctx.trade != nullptr) {
            on_trade(ctx, *ctx.trade);
        }
        if (closing_) {
            close(ctx);
        }
    }

    void on_report(StrategyContext&, const ExecutionReport& report) noexcept
    {
        if (report.order_id != in_flight_) {
            return;
        }
        if (report.type == ReportType::Fill) {
            (report.side == Side::Buy ? stats_.bought : stats_.sold) += report.quantity.value();
        } else if (report.type == ReportType::Expired || report.type == ReportType::Rejected) {
            stats_.expired += report.quantity.value();
        }
        if (report.type != ReportType::Accepted && report.leaves.is_zero()) {
            in_flight_ = 0;
        }
    }

    void on_timer(StrategyContext& ctx, std::uint64_t) noexcept
    {
        if (sessions_.empty()) {
            return;
        }
        sync_day(ctx);
        on_clock(ctx);
        if (closing_) {
            close(ctx);
        }
    }

    const OrbStats& stats() const noexcept
    {
        return stats_;
    }
};

static_assert(Strategy<Orb>);

} // namespace simulator
