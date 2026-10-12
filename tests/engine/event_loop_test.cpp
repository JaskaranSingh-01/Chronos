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

namespace simulator {
namespace {

constexpr SymbolId kSymbol = 3;
constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};
constexpr RiskLimits kWide{.max_position = 1'000'000, .max_order_quantity = Quantity{1'000'000},
                           .max_open_orders = 16};

// The strategy can only read the historical book.
static_assert(std::is_same_v<decltype(StrategyContext::book), const MarketBook&>);
static_assert(std::is_same_v<decltype(StrategyContext::portfolio), const Portfolio&>);

MarketEvent add(Timestamp ts, OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts, .order_id = id, .price = Price{price}, .quantity = Quantity{quantity},
                       .symbol = kSymbol, .type = MarketEventType::Add, .side = side};
}

MarketEvent execute(Timestamp ts, OrderId id, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts, .order_id = id, .quantity = Quantity{quantity}, .symbol = kSymbol,
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
        Seen s{.kind = kind, .now = ctx.now(), .position = ctx.portfolio.position(),
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
        : source(events), portfolio(kEquity, limits, fees) {}

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
    EXPECT_EQ(report.discarded_sim_events, 0u);
}

TEST(EventLoopTest, StrategySeesPostApplyBookAndMarkedPortfolio)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events};
    buy_at_twenty(h);

    h.run({.order_latency = 100, .report_latency = 100});
    ASSERT_EQ(h.strategy.seen.size(), 5u);

    // ② before ④: the callback for an Add already sees it.
    EXPECT_EQ(h.strategy.seen[0].bid, (LevelInfo{Price{999'900}, 100, 1}));
    EXPECT_EQ(h.strategy.seen[1].ask, (LevelInfo{Price{1'000'000}, 300, 1}));

    // The portfolio has the fill before the strategy hears of it.
    EXPECT_EQ(h.strategy.seen[3].position, 100);

    // ③ before ④: the long is already marked at the new best bid.
    const Seen& last = h.strategy.seen[4];
    EXPECT_EQ(last.bid.price, Price{999'950});
    EXPECT_EQ(last.unrealised, 100 * (999'950 - 1'000'000));
    // ① (ExecutionSimulator::on_market) is a no-op until resting orders exist.
}

// Spec §5 walkthrough, with the t=1,000 execution taking the level down to
// 100 so the tie decides the fill: the market event at 21,000 refills the
// ask to 200 before our order arrives at 21,000.
TEST(EventLoopTest, TieGoesToMarketInTheSpecWalkthrough)
{
    const std::vector<MarketEvent> events{
        add(0, 1, Side::Buy, 1'000'000, 100),
        add(0, 7, Side::Sell, 1'000'100, 500),
        execute(1'000, 7, 400),
        add(21'000, 9, Side::Sell, 1'000'100, 100),
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
              (ExecutionReport{.exchange_time = 21'000, .order_id = 1, .price = Price{1'000'100},
                               .quantity = Quantity{200}, .leaves = Quantity{0}, .type = ReportType::Fill,
                               .liquidity = Liquidity::Taker, .side = Side::Buy}));

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

TEST(EventLoopTest, PendingSimEventsAreDiscardedAtEndOfStream)
{
    const std::vector<MarketEvent> events = round_trip_events();
    Harness h{events};
    h.strategy.act = [](StrategyContext& ctx, const MarketEvent& event) {
        if (event.timestamp == 300) {
            OrderId id{0};
            ctx.gateway.send(Side::Sell, Price{999'950}, Quantity{10}, TimeInForce::Ioc, id);
        }
    };

    const LoopReport report = h.run({.order_latency = 100, .report_latency = 100});

    EXPECT_TRUE(report.replay.succeeded());
    EXPECT_EQ(report.sim_events, 0u);
    EXPECT_EQ(report.discarded_sim_events, 1u);
    EXPECT_EQ(h.portfolio.position(), 0);
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
            events.push_back(MarketEvent{.timestamp = ts, .order_id = id, .symbol = kSymbol,
                                         .type = MarketEventType::Delete});
            break;
        default:
            events.push_back(MarketEvent{.timestamp = ts, .price = Price{95}, .quantity = Quantity{10},
                                         .symbol = kSymbol, .type = MarketEventType::Trade});
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
    explicit FailingSource(std::span<const MarketEvent> events) : events_(events) {}

    SourceResult next(MarketEvent& event) noexcept override
    {
        return events_.next(event) == SourceResult::Event ? SourceResult::Event : SourceResult::Error;
    }
    std::uint64_t position() const noexcept override { return events_.position(); }
    SourceError error() const noexcept override { return SourceError::TruncatedMessage; }
};

TEST(EventLoopTest, FatalFailuresMatchMarketReplay)
{
    const std::vector<MarketEvent> out_of_order{add(10, 1, Side::Buy, 100, 5), add(20, 2, Side::Sell, 101, 5),
                                                add(15, 3, Side::Buy, 99, 5), add(30, 4, Side::Buy, 98, 5)};
    const std::vector<MarketEvent> clean{add(10, 1, Side::Buy, 100, 5), add(20, 2, Side::Sell, 101, 5)};

    auto compare = [](auto make_source, std::span<const MarketEvent> events, ReplayFailure failure) {
        auto replay_source = make_source(events);
        MarketBook replay_book{kSymbol, 8};
        const ReplayReport expected = MarketReplay::run(replay_source, replay_book, {});

        auto source = make_source(events);
        MarketBook book{kSymbol, 8};
        ExecutionSimulator execution;
        Portfolio portfolio{kEquity, kWide, FeeSchedule{}};
        NullStrategy strategy;
        EventLoop<NullStrategy> loop{source, book, execution, portfolio, strategy, LatencyConfig{}};
        const LoopReport report = loop.run();

        EXPECT_EQ(expected.failure, failure);
        EXPECT_EQ(report.replay, expected);
        EXPECT_EQ(book.digest(), replay_book.digest());
    };

    compare([](std::span<const MarketEvent> e) { return VectorSource{e}; }, out_of_order,
            ReplayFailure::OutOfOrderTimestamp);
    compare([](std::span<const MarketEvent> e) { return FailingSource{e}; }, clean, ReplayFailure::SourceError);
}

} // namespace
} // namespace simulator
