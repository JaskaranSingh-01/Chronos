#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

#include "simulator/engine/event_loop.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "simulator/strategies/market_maker.hpp"

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 3;
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr RiskLimits kWide{.max_position = 1'000'000, .max_order_quantity = Quantity{1'000'000}, .max_open_orders = 16};

// The strategy can only read the historical book.
static_assert(std::is_same_v<decltype(StrategyContext::book), const MarketBook&>);
static_assert(std::is_same_v<decltype(StrategyContext::portfolio), const Portfolio&>);

MarketEvent add(Timestamp ts, OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Add,
                       .side = side};
}

MarketEvent execute(Timestamp ts, OrderId id, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Execute};
}

// What a callback could see when it ran.
struct Seen
{
    char kind{'M'};  // Market, Report, Timer
    Timestamp now{0};
    LevelInfo bid{};  // zero if that side is empty
    LevelInfo ask{};
    std::int64_t position{0};
    std::int64_t unrealised{0};
    ExecutionReport report{};
    std::uint64_t timer_id{0};
};

// Records every callback; `act` runs after on_market's record.
struct Recorder
{
    std::vector<Seen> seen;
    std::function<void(StrategyContext&, const MarketEvent&)> act;

    static Seen snapshot(const StrategyContext& ctx, char kind)
    {
        Seen s{.kind = kind,
               .now = ctx.now(),
               .position = ctx.portfolio.position(),
               .unrealised = ctx.portfolio.unrealised_pnl()};
        ctx.book.best(Side::Buy, s.bid);
        ctx.book.best(Side::Sell, s.ask);
        return s;
    }

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        seen.push_back(snapshot(ctx, 'M'));
        if (act) {
            act(ctx, event);
        }
    }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        Seen s = snapshot(ctx, 'R');
        s.report = report;
        seen.push_back(s);
    }

    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept
    {
        Seen s = snapshot(ctx, 'T');
        s.timer_id = timer_id;
        seen.push_back(s);
    }

    std::vector<char> kinds() const
    {
        std::vector<char> out;
        for (const Seen& s : seen) {
            out.push_back(s.kind);
        }
        return out;
    }
};

static_assert(Strategy<Recorder>);

struct Harness
{
    VectorSource source;
    MarketBook book{kSymbol, 64};
    ExecutionSimulator execution;
    Portfolio portfolio;
    Recorder strategy;

    explicit Harness(std::span<const MarketEvent> events, RiskLimits limits = kWide, FeeSchedule fees = {})
        : source(events), portfolio(kEquity, limits, fees)
    {}

    LoopReport run(LatencyConfig latency)
    {
        EventLoop<Recorder> loop{source, book, execution, portfolio, strategy, latency};
        return loop.run();
    }
};

// Bid 999,900 x 100 at t=10, ask 1,000,000 x 300 at t=20, then (t=300) a
// better bid 999,950 x 50 after our round trip.
std::vector<MarketEvent> round_trip_events()
{
    return {add(10, 1, Side::Buy, 999'900, 100), add(20, 2, Side::Sell, 1'000'000, 300),
            add(300, 3, Side::Buy, 999'950, 50)};
}

// Buys 100 at the ask on the t=20 event and arms timer 9 for t=70.
void buy_at_twenty(Harness& h)
{
    h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 20) {
            OrderId id{0};
            ASSERT_EQ(ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{100}, TimeInForce::Ioc, id),
                      RiskCheck::Accepted);
            ASSERT_TRUE(ctx.set_timer(70, 9));
        }
    };
}

TEST(EventLoopTest, LatencySchedulesArrivalAndDelivery)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events};
    buy_at_twenty(h);

    const LoopReport report = h.run({.order_latency = 100, .report_latency = 100});

    ASSERT_TRUE(report.replay.succeeded());
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'T', 'R', 'M'}));
    const Seen& timer = h.strategy.seen[2];
    EXPECT_EQ(timer.now, 70);
    EXPECT_EQ(timer.timer_id, 9u);

    const Seen& delivered = h.strategy.seen[3];
    EXPECT_EQ(delivered.report.exchange_time, 120);  // arrived at 20 + order_latency
    EXPECT_EQ(delivered.now, 220);                   // delivered report_latency later
    EXPECT_EQ(delivered.report.type, ReportType::Fill);
    EXPECT_EQ(delivered.report.order_id, 1u);
    EXPECT_EQ(delivered.report.quantity, Quantity{100});
    EXPECT_EQ(report.sim_events, 3u);  // timer, arrival, delivery
    EXPECT_EQ(report.drained_sim_events, 0u);
    EXPECT_EQ(report.discarded_sim_events, 0u);
}

TEST(EventLoopTest, StrategySeesPostApplyBookAndMarkedPortfolio)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events};
    buy_at_twenty(h);

    h.run({.order_latency = 100, .report_latency = 100});
    ASSERT_EQ(h.strategy.seen.size(), 5u);

    // The book applies an Add before the callback, which already sees it.
    EXPECT_EQ(h.strategy.seen[0].bid, (LevelInfo{Price{999'900}, 100, 1}));
    EXPECT_EQ(h.strategy.seen[1].ask, (LevelInfo{Price{1'000'000}, 300, 1}));

    // The portfolio has the fill before the strategy hears of it.
    EXPECT_EQ(h.strategy.seen[3].position, 100);

    // The mark runs before the callback: the long is already at the new best bid.
    const Seen& last = h.strategy.seen[4];
    EXPECT_EQ(last.bid.price, Price{999'950});
    EXPECT_EQ(last.unrealised, 100 * (999'950 - 1'000'000));
}

// The t=1,000 execution takes the level down to 100, so the tie decides the
// fill: the market event at 21,000 refills the ask to 200 before our order
// arrives at 21,000.
TEST(EventLoopTest, TieGoesToMarketInTheSpecWalkthrough)
{
    const std::vector<MarketEvent> events{add(0, 1, Side::Buy, 1'000'000, 100), add(0, 7, Side::Sell, 1'000'100, 500),
                                          execute(1'000, 7, 400), add(21'000, 9, Side::Sell, 1'000'100, 100),
                                          add(50'000, 10, Side::Buy, 999'900, 100)};
    Harness h{events, kWide, FeeSchedule{.taker_fee_per_share = 30}};
    h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 1'000) {
            OrderId id{0};
            ASSERT_EQ(ctx.gateway.send(Side::Buy, Price{1'000'100}, Quantity{200}, TimeInForce::Ioc, id),
                      RiskCheck::Accepted);
        }
    };

    const LoopReport report = h.run({.order_latency = 20'000, .report_latency = 15'000});

    ASSERT_TRUE(report.replay.succeeded());
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'M', 'M', 'R', 'M'}));
    EXPECT_EQ(h.strategy.seen[3].ask.quantity, 200u);  // the tie's callback came first

    const Seen& delivered = h.strategy.seen[4];
    EXPECT_EQ(delivered.now, 36'000);
    EXPECT_EQ(delivered.report,
              (ExecutionReport{.exchange_time = 21'000,
                               .order_id = 1,
                               .price = Price{1'000'100},
                               .quantity = Quantity{200},
                               .leaves = Quantity{0},
                               .type = ReportType::Fill,
                               .liquidity = Liquidity::Taker,
                               .side = Side::Buy}));

    EXPECT_EQ(h.portfolio.position(), 200);
    EXPECT_EQ(h.portfolio.cash(), -200'020'000);
    EXPECT_EQ(h.portfolio.fees(), 6'000);
    EXPECT_EQ(h.portfolio.open_orders(), 0u);
    EXPECT_EQ(h.book.quantity_at(Side::Sell, Price{1'000'100}), 200u);  // no market impact
}

TEST(EventLoopTest, RiskRejectSchedulesNothing)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events, RiskLimits{.max_position = 1'000, .max_order_quantity = Quantity{50}, .max_open_orders = 1}};
    RiskCheck check{RiskCheck::Accepted};
    h.strategy.act = [&check](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 20) {
            OrderId id{0};
            check = ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{100}, TimeInForce::Ioc, id);
        }
    };

    const LoopReport report = h.run({.order_latency = 100, .report_latency = 100});

    EXPECT_EQ(check, RiskCheck::OrderTooLarge);
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'M'}));
    EXPECT_EQ(report.sim_events, 0u);
    EXPECT_EQ(report.discarded_sim_events, 0u);
    EXPECT_EQ(h.portfolio.open_orders(), 0u);
}

// Sells 10 at the best bid (999,950 x 50) on the last market event, t=300.
void sell_at_the_end(Harness& h)
{
    h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 300) {
            OrderId id{0};
            ASSERT_EQ(ctx.gateway.send(Side::Sell, Price{999'950}, Quantity{10}, TimeInForce::Ioc, id),
                      RiskCheck::Accepted);
        }
    };
}

// The order arrives at 400 and its report is delivered at 500, both after the
// last market event: the drain still executes it against the final book and
// delivers the fill to the Portfolio and the strategy.
TEST(EventLoopTest, InFlightOrderAndReportAreDrainedAtEndOfStream)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events};
    sell_at_the_end(h);

    const LoopReport report = h.run({.order_latency = 100, .report_latency = 100});

    ASSERT_TRUE(report.replay.succeeded());
    EXPECT_EQ(report.sim_events, 2u);  // arrival, delivery
    EXPECT_EQ(report.drained_sim_events, 2u);
    EXPECT_EQ(report.discarded_sim_events, 0u);
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'M', 'R'}));

    const Seen& delivered = h.strategy.seen[3];
    EXPECT_EQ(delivered.now, 500);  // the clock moves on; the book stays frozen
    EXPECT_EQ(delivered.bid, (LevelInfo{Price{999'950}, 50, 1}));
    EXPECT_EQ(delivered.report,
              (ExecutionReport{.exchange_time = 400,
                               .order_id = 1,
                               .price = Price{999'950},
                               .quantity = Quantity{10},
                               .leaves = Quantity{0},
                               .type = ReportType::Fill,
                               .liquidity = Liquidity::Taker,
                               .side = Side::Sell}));
    EXPECT_EQ(delivered.position, -10);

    EXPECT_EQ(h.portfolio.position(), -10);
    EXPECT_EQ(h.portfolio.cash(), 9'999'500);
    EXPECT_EQ(h.portfolio.open_orders(), 0u);
}

// A resting Day bid: Accepted, a Maker fill from a crossing Add (rule c) with a
// rebate, then a gateway cancel, each delivered report_latency later.
TEST(EventLoopTest, RestingOrderFillsAsMakerAndCancelsEndToEnd)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Buy, 999'900, 100), add(20, 2, Side::Sell, 1'000'100, 100),
                                          add(500, 3, Side::Sell, 1'000'000, 30),
                                          add(1'000, 4, Side::Buy, 999'800, 10)};
    Harness h{events, kWide, FeeSchedule{.maker_fee_per_share = -2, .taker_fee_per_share = 30}};
    h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        OrderId id{0};
        if (event.timestamp == 20) {
            ASSERT_EQ(ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{50}, TimeInForce::Day, id),
                      RiskCheck::Accepted);
        } else if (event.timestamp == 1'000) {
            ctx.gateway.cancel(1);
        }
    };

    const LoopReport report = h.run({.order_latency = 100, .report_latency = 100});

    ASSERT_TRUE(report.replay.succeeded());
    ASSERT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'R', 'M', 'R', 'M', 'R'}));
    EXPECT_EQ(h.strategy.seen[2].now, 220);
    EXPECT_EQ(h.strategy.seen[2].report.type, ReportType::Accepted);
    EXPECT_EQ(h.strategy.seen[4].now, 600);
    EXPECT_EQ(h.strategy.seen[4].report,
              (ExecutionReport{.exchange_time = 500,
                               .order_id = 1,
                               .price = Price{1'000'000},
                               .quantity = Quantity{30},
                               .leaves = Quantity{20},
                               .type = ReportType::Fill,
                               .liquidity = Liquidity::Maker,
                               .side = Side::Buy}));
    EXPECT_EQ(h.strategy.seen[6].now, 1'200);  // drained after the last market event
    EXPECT_EQ(h.strategy.seen[6].report,
              (ExecutionReport{.exchange_time = 1'100,
                               .order_id = 1,
                               .quantity = Quantity{20},
                               .type = ReportType::Cancelled,
                               .side = Side::Buy}));

    EXPECT_EQ(h.portfolio.position(), 30);
    EXPECT_EQ(h.portfolio.cash(), -30'000'000);
    EXPECT_EQ(h.portfolio.fees(), -60);  // a rebate
    EXPECT_EQ(h.portfolio.open_orders(), 0u);
    EXPECT_EQ(h.portfolio.open_quantity(Side::Buy), 0u);
    EXPECT_EQ(h.execution.open_orders(), 0u);
}

// Re-sends an IOC that never fills on every report, forever, and re-arms a
// timer on every timer.
struct PingPong
{
    std::uint64_t reports{0};
    std::uint64_t timers{0};
    bool rearmed{true};

    void send(StrategyContext& ctx) noexcept
    {
        OrderId id{0};
        ctx.gateway.send(Side::Buy, Price{1}, Quantity{1}, TimeInForce::Ioc, id);
    }
    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        if (event.timestamp == 300) {
            send(ctx);
            ctx.set_timer(ctx.now(), 1);
        }
    }
    void on_report(StrategyContext& ctx, const ExecutionReport&) noexcept
    {
        ++reports;
        send(ctx);
    }
    void on_timer(StrategyContext& ctx, std::uint64_t) noexcept
    {
        ++timers;
        rearmed = ctx.set_timer(ctx.now() + 1, 1);
    }
};

static_assert(Strategy<PingPong>);

TEST(EventLoopTest, DrainRefusesNewTimersAndStopsAtItsCap)
{
    const std::vector<MarketEvent> events = round_trip_events();
    VectorSource source{events};
    MarketBook book{kSymbol, 64};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    PingPong strategy;
    EventLoop<PingPong> loop{source, book, execution, portfolio, strategy, {.order_latency = 1, .report_latency = 1}};

    const LoopReport report = loop.run();

    ASSERT_TRUE(report.replay.succeeded());
    EXPECT_EQ(strategy.timers, 1u);  // armed before the end, fired in the drain
    EXPECT_FALSE(strategy.rearmed);  // refused after the end
    EXPECT_EQ(report.drained_sim_events, max_drain_events);
    EXPECT_EQ(report.sim_events, max_drain_events);
    EXPECT_EQ(report.discarded_sim_events, 1u);  // the ping-pong's next event
    EXPECT_EQ(strategy.reports, max_drain_events / 2 - 1);  // every other event, minus the timer
}

// A fixed-seed stream of valid and rejected events, like MarketReplayTest's.
std::vector<MarketEvent> random_stream()
{
    std::mt19937_64 rng{7};
    std::vector<MarketEvent> events;
    OrderId next_id = 1;

    for (Timestamp ts = 0; ts < 5'000; ts += static_cast<Timestamp>(rng() % 3)) {
        const OrderId id = 1 + rng() % next_id;  // sometimes unknown or gone
        switch (rng() % 5) {
        case 0:
        case 1: {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            const auto price = 90 + static_cast<std::int64_t>(rng() % 20);
            events.push_back(add(ts, next_id++, side, price, 1 + rng() % 500));
            break;
        }
        case 2:
            events.push_back(execute(ts, id, 1 + rng() % 100));
            break;
        case 3:
            events.push_back(
                MarketEvent{.timestamp = ts, .order_id = id, .symbol = kSymbol, .type = MarketEventType::Delete});
            break;
        default:
            events.push_back(MarketEvent{.timestamp = ts,
                                         .price = Price{95},
                                         .quantity = Quantity{10},
                                         .symbol = kSymbol,
                                         .type = MarketEventType::Trade});
        }
    }
    return events;
}

TEST(EventLoopTest, NullStrategyMatchesMarketReplay)
{
    const std::vector<MarketEvent> events = random_stream();
    const ReplayOptions options{.check_invariants_every = 100};

    VectorSource replay_source{events};
    MarketBook replay_book{kSymbol, 8'192};
    const ReplayReport expected = MarketReplay::run(replay_source, replay_book, options);

    VectorSource source{events};
    MarketBook book{kSymbol, 8'192};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    NullStrategy strategy;
    EventLoop<NullStrategy> loop{source, book, execution, portfolio, strategy, LatencyConfig{}, options};
    const LoopReport report = loop.run();

    ASSERT_TRUE(expected.succeeded());
    EXPECT_GT(expected.rejected(), 0u);
    EXPECT_GT(expected.invariant_checks, 0u);
    EXPECT_EQ(report.replay, expected);
    EXPECT_EQ(book.digest(), replay_book.digest());
    EXPECT_EQ(report.sim_events, 0u);
}

// Emits its events, then fails.
class FailingSource final : public MarketEventSource
{
    VectorSource events_;

public:
    explicit FailingSource(std::span<const MarketEvent> events)
        : events_(events)
    {}

    SourceResult next(MarketEvent& event) noexcept override
    {
        return events_.next(event) == SourceResult::Event ? SourceResult::Event : SourceResult::Error;
    }
    std::uint64_t position() const noexcept override
    {
        return events_.position();
    }
    SourceError error() const noexcept override
    {
        return SourceError::TruncatedMessage;
    }
};

TEST(EventLoopTest, FatalFailuresMatchMarketReplay)
{
    const std::vector<MarketEvent> out_of_order{add(10, 1, Side::Buy, 100, 5), add(20, 2, Side::Sell, 101, 5),
                                                add(15, 3, Side::Buy, 99, 5), add(30, 4, Side::Buy, 98, 5)};
    const std::vector<MarketEvent> clean{add(10, 1, Side::Buy, 100, 5), add(20, 2, Side::Sell, 101, 5)};
    // A bid above the ask in RTH: the invariant check after the second Add fails.
    constexpr Timestamp midnight = 1'577'682'000'000'000'000;  // 2019-12-30, -05:00
    const std::vector<MarketEvent> crossed_in_rth{add(midnight + RTH_OPEN_NS, 1, Side::Buy, 101, 5),
                                                  add(midnight + RTH_OPEN_NS, 2, Side::Sell, 100, 5)};

    auto compare = [](auto make_source, std::span<const MarketEvent> events, const ReplayOptions& options,
                      ReplayFailure failure) {
        auto replay_source = make_source(events);
        MarketBook replay_book{kSymbol, 8};
        const ReplayReport expected = MarketReplay::run(replay_source, replay_book, options);

        auto source = make_source(events);
        MarketBook book{kSymbol, 8};
        ExecutionSimulator execution;
        Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
        NullStrategy strategy;
        EventLoop<NullStrategy> loop{source, book, execution, portfolio, strategy, LatencyConfig{}, options};
        const LoopReport report = loop.run();

        EXPECT_EQ(expected.failure, failure);
        EXPECT_EQ(report.replay, expected);
        EXPECT_EQ(book.digest(), replay_book.digest());
    };

    compare([](std::span<const MarketEvent> e) { return VectorSource{e}; }, out_of_order, {},
            ReplayFailure::OutOfOrderTimestamp);
    compare([](std::span<const MarketEvent> e) { return FailingSource{e}; }, clean, {}, ReplayFailure::SourceError);
    compare([](std::span<const MarketEvent> e) { return VectorSource{e}; }, crossed_in_rth,
            {.check_invariants_every = 1, .session_midnight = midnight}, ReplayFailure::InvariantViolation);
}

TEST(EventLoopTest, NoDrainAfterAFatalFailure)
{
    const std::vector<MarketEvent> events = round_trip_events();
    FailingSource source{events};
    MarketBook book{kSymbol, 64};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    Recorder strategy;
    strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 300) {
            OrderId id{0};
            ctx.gateway.send(Side::Sell, Price{999'950}, Quantity{10}, TimeInForce::Ioc, id);
        }
    };
    EventLoop<Recorder> loop{source,    book,     execution,
                             portfolio, strategy, {.order_latency = 100, .report_latency = 100}};

    const LoopReport report = loop.run();

    EXPECT_EQ(report.replay.failure, ReplayFailure::SourceError);
    EXPECT_EQ(report.sim_events, 0u);
    EXPECT_EQ(report.drained_sim_events, 0u);
    EXPECT_EQ(report.discarded_sim_events, 1u);
    EXPECT_EQ(portfolio.position(), 0);
}

// Drives `loop` by hand; with `advance_first`, also advances to just before every market event.
template <class Loop>
void drive(Loop& loop, bool advance_first)
{
    Timestamp next_ts{0};
    while (loop.peek(next_ts) == PeekStatus::Event) {
        if (advance_first && next_ts > 0) {
            loop.advance_to(next_ts - 1);
        }
        if (!loop.step().ok) {
            break;
        }
    }
    loop.finish();
}

// random_stream with bids below 100 and asks above, so a quoting strategy trades.
std::vector<MarketEvent> uncrossed_stream()
{
    std::vector<MarketEvent> events = random_stream();
    for (MarketEvent& event : events) {
        if (event.type == MarketEventType::Add) {
            event.price = Price{event.side == Side::Buy ? 90 + event.price.ticks % 10 : 101 + event.price.ticks % 10};
        }
    }
    return events;
}

struct MarketMakerRun
{
    LoopReport report;
    std::uint64_t digest{0};
    std::int64_t position{0};
    std::int64_t cash{0};

    bool operator==(const MarketMakerRun&) const = default;
};

// mode 0: run(); 1: peek/step; 2: peek/advance_to/step.
MarketMakerRun run_market_maker(std::span<const MarketEvent> events, LatencyConfig latency, int mode)
{
    VectorSource source{events};
    MarketBook book{kSymbol, 8'192};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
    MarketMaker strategy{MarketMakerParams{.quote_size = 10, .max_position = 1'000, .requote_cooldown_ns = 0}};
    EventLoop<MarketMaker> loop{source, book, execution, portfolio, strategy, latency, {.check_invariants_every = 100}};
    if (mode == 0) {
        loop.run();
    } else {
        drive(loop, mode == 2);
    }
    return {loop.report(), book.digest(), portfolio.position(), portfolio.cash()};
}

TEST(EventLoopTest, StepApiMatchesRunWithAQuotingStrategy)
{
    const std::vector<MarketEvent> events = uncrossed_stream();
    for (const LatencyConfig latency : {LatencyConfig{0, 0}, LatencyConfig{1, 1}, LatencyConfig{3, 2}}) {
        const MarketMakerRun expected = run_market_maker(events, latency, 0);
        ASSERT_TRUE(expected.report.replay.succeeded());
        EXPECT_GT(expected.report.sim_events, 0u);  // the strategy really traded
        EXPECT_EQ(run_market_maker(events, latency, 1), expected);
        EXPECT_EQ(run_market_maker(events, latency, 2), expected);
    }
}

// The order arrives at 100 with zero latency, but only after the delete at 100.
TEST(EventLoopTest, SimEventsAtATimestampWaitForEveryMarketEventAtIt)
{
    const std::vector<MarketEvent> events{
        add(100, 1, Side::Sell, 1'000'000, 100),
        MarketEvent{.timestamp = 100, .order_id = 1, .symbol = kSymbol, .type = MarketEventType::Delete}};

    for (const bool stepped : {false, true}) {
        Harness h{events};
        h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
            if (event.type == MarketEventType::Add) {
                OrderId id{0};
                ASSERT_EQ(ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{100}, TimeInForce::Ioc, id),
                          RiskCheck::Accepted);
            }
        };
        if (stepped) {
            EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
            drive(loop, false);
        } else {
            h.run(LatencyConfig{});
        }

        ASSERT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'R'}));
        EXPECT_EQ(h.strategy.seen[2].report.type, ReportType::Expired);
        EXPECT_EQ(h.strategy.seen[2].report.exchange_time, 100);
    }
}

std::vector<MarketEvent> two_far_events()
{
    return {add(10, 1, Side::Sell, 1'000'000, 100), add(1'000, 2, Side::Buy, 999'900, 100)};
}

// Sends an IOC buy of `quantity` at 1,000,000 on the first event.
void buy_on_first_event(Harness& h, std::uint64_t quantity)
{
    h.strategy.act = [quantity](StrategyContext& ctx, const MarketEvent& event) {
        if (event.order_id == 1) {
            OrderId id{0};
            ASSERT_EQ(ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{quantity}, TimeInForce::Ioc, id),
                      RiskCheck::Accepted);
        }
    };
}

TEST(EventLoopTest, AdvanceToRunsDueSimEventsBeforeTheNextMarketEvent)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    buy_on_first_event(h, 50);
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 100, .report_latency = 100}};
    Timestamp next_ts{0};
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);
    ASSERT_EQ(next_ts, 1'000);

    loop.advance_to(500);

    ASSERT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'R'}));
    EXPECT_EQ(h.strategy.seen[1].now, 210);  // arrived at 110, delivered at 210
    EXPECT_EQ(h.strategy.seen[1].report.type, ReportType::Fill);
    EXPECT_EQ(h.strategy.seen[1].report.quantity, Quantity{50});
    EXPECT_EQ(loop.gateway().now(), 500);
    EXPECT_EQ(loop.report().replay.events, 1u);  // the second event is peeked, not yet counted
}

TEST(EventLoopTest, AdvanceToHoldsSimEventsAtTheNextMarketTimestamp)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    buy_on_first_event(h, 50);
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 990, .report_latency = 0}};  // arrives at 1,000
    Timestamp next_ts{0};
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);

    loop.advance_to(999);
    loop.advance_to(1'000);

    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M'}));
    EXPECT_EQ(loop.gateway().now(), 999);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::EndOfStream);  // finish() drains only once the stream has ended
    loop.finish();
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'R'}));  // the market event at 1,000 first
}

TEST(EventLoopTest, CommandsBetweenStepsUseTheAdvancedClock)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 100, .report_latency = 100}};
    Timestamp next_ts{0};
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(next_ts), PeekStatus::Event);
    loop.advance_to(500);

    OrderId id{0};
    ASSERT_EQ(loop.gateway().send(Side::Buy, Price{1'000'000}, Quantity{10}, TimeInForce::Ioc, id),
              RiskCheck::Accepted);
    loop.advance_to(900);

    ASSERT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'R'}));
    EXPECT_EQ(h.strategy.seen[1].report.exchange_time, 600);
    EXPECT_EQ(h.strategy.seen[1].now, 700);
}

TEST(EventLoopTest, PeekSeparatesEndOfStreamFromError)
{
    {
        const std::vector<MarketEvent> clean{add(10, 1, Side::Buy, 100, 5)};
        Harness h{clean};
        EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
        Timestamp ts{0};
        ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
        ASSERT_TRUE(loop.step().ok);
        EXPECT_EQ(loop.peek(ts), PeekStatus::EndOfStream);
        EXPECT_EQ(loop.peek(ts), PeekStatus::EndOfStream);
        loop.finish();
        EXPECT_TRUE(loop.report().replay.succeeded());
    }
    {
        const std::vector<MarketEvent> out_of_order{add(10, 1, Side::Sell, 1'000'000, 100),
                                                    add(5, 2, Side::Buy, 99, 5)};
        Harness h{out_of_order};
        buy_on_first_event(h, 10);
        EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                                 h.portfolio, h.strategy, {.order_latency = 1'000, .report_latency = 0}};
        Timestamp ts{0};
        ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
        ASSERT_TRUE(loop.step().ok);
        EXPECT_EQ(loop.peek(ts), PeekStatus::Error);
        EXPECT_EQ(loop.report().replay.failure, ReplayFailure::OutOfOrderTimestamp);
        loop.finish();
        EXPECT_EQ(loop.report().drained_sim_events, 0u);
        EXPECT_EQ(loop.report().discarded_sim_events, 1u);  // the arrival never runs after a fatal failure
    }
}

TEST(EventLoopTest, PeekTwiceReadsOneEvent)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Buy, 100, 5)};
    Harness h{events};
    EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
    Timestamp first{0};
    Timestamp second{0};
    ASSERT_EQ(loop.peek(first), PeekStatus::Event);
    ASSERT_EQ(loop.peek(second), PeekStatus::Event);
    EXPECT_EQ(first, second);
    ASSERT_TRUE(loop.step().ok);
    EXPECT_EQ(loop.report().replay.events, 1u);
    EXPECT_EQ(loop.peek(first), PeekStatus::EndOfStream);
}

TEST(EventLoopTest, StepWithoutAPendingEventDoesNothing)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Buy, 100, 5)};
    Harness h{events};
    EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
    EXPECT_FALSE(loop.step().ok);  // no peek yet
    EXPECT_EQ(loop.report().replay.events, 0u);

    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(ts), PeekStatus::EndOfStream);
    EXPECT_FALSE(loop.step().ok);
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M'}));
}

TEST(EventLoopTest, AdvanceToNeverMovesTheClockBackwards)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.gateway().now(), 10);

    loop.advance_to(5);

    EXPECT_EQ(loop.gateway().now(), 10);
}

TEST(EventLoopTest, AdvanceToAfterEndOfStreamRunsLateArrivals)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Sell, 1'000'000, 100)};
    Harness h{events};
    buy_on_first_event(h, 10);
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 100, .report_latency = 0}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(ts), PeekStatus::EndOfStream);

    loop.advance_to(200);

    ASSERT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'R'}));
    EXPECT_EQ(h.strategy.seen[1].report.type, ReportType::Fill);
    loop.finish();
    EXPECT_EQ(loop.report().drained_sim_events, 0u);
}

TEST(EventLoopTest, EarlyFinishDiscardsAndFinishIsIdempotent)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    buy_on_first_event(h, 10);
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 100, .report_latency = 0}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);  // the event at 1,000 is pending

    loop.finish();

    EXPECT_EQ(loop.report().drained_sim_events, 0u);
    EXPECT_EQ(loop.report().discarded_sim_events, 1u);  // the arrival at 110, never run
    EXPECT_EQ(loop.report().replay.rejected(), 0u);  // the peeked event is not counted as read
    const LoopReport first = loop.report();
    loop.finish();
    EXPECT_EQ(loop.report(), first);
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M'}));
}

TEST(EventLoopTest, AdvanceToWithoutPeekStillHoldsAtTheNextMarketEvent)
{
    const std::vector<MarketEvent> events = two_far_events();
    Harness h{events};
    buy_on_first_event(h, 50);
    EventLoop<Recorder> loop{h.source,    h.book,
                             h.execution, h.portfolio,
                             h.strategy,  {.order_latency = 1'500, .report_latency = 0}};  // arrives at 1,510
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);

    loop.advance_to(2'000);  // the event at 1,000 has not been peeked

    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M'}));
    EXPECT_EQ(loop.gateway().now(), 10);
    ASSERT_TRUE(loop.step().ok);
    EXPECT_EQ(loop.gateway().now(), 1'000);
    ASSERT_EQ(loop.peek(ts), PeekStatus::EndOfStream);
    loop.advance_to(2'000);
    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M', 'M', 'R'}));
    EXPECT_EQ(loop.gateway().now(), 2'000);
}

TEST(EventLoopTest, TimersAreOffOnceTheStreamHasEnded)
{
    const std::vector<MarketEvent> events{add(10, 1, Side::Buy, 100, 5)};
    Harness h{events};
    EventLoop<Recorder> loop{h.source, h.book, h.execution, h.portfolio, h.strategy, LatencyConfig{}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);
    EXPECT_TRUE(loop.gateway().set_timer(50, 1));  // the end of the stream has not been seen yet

    ASSERT_EQ(loop.peek(ts), PeekStatus::EndOfStream);

    EXPECT_FALSE(loop.gateway().set_timer(60, 2));
}

TEST(EventLoopTest, AdvanceToRunsNothingWhenItsPeekFindsAFatalFailure)
{
    const std::vector<MarketEvent> out_of_order{add(10, 1, Side::Sell, 1'000'000, 100), add(5, 2, Side::Buy, 99, 5)};
    Harness h{out_of_order};
    buy_on_first_event(h, 10);
    EventLoop<Recorder> loop{h.source,    h.book,     h.execution,
                             h.portfolio, h.strategy, {.order_latency = 1, .report_latency = 0}};
    Timestamp ts{0};
    ASSERT_EQ(loop.peek(ts), PeekStatus::Event);
    ASSERT_TRUE(loop.step().ok);

    loop.advance_to(1'000);  // reads the event at 5, which is out of order

    EXPECT_EQ(h.strategy.kinds(), (std::vector<char>{'M'}));  // the arrival at 11 did not run
    EXPECT_EQ(loop.gateway().now(), 10);
    EXPECT_EQ(loop.report().replay.failure, ReplayFailure::OutOfOrderTimestamp);
    loop.finish();
    EXPECT_EQ(loop.report().drained_sim_events, 0u);
    EXPECT_EQ(loop.report().discarded_sim_events, 1u);
}

}  // namespace
}  // namespace simulator
