#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "chronos/server/generation.hpp"
#include "simulator/market/vector_source.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

// A generation over `market`, which it keeps alive.
struct Fixture
{
    std::vector<MarketEvent> events;
    Generation g;

    explicit Fixture(std::vector<MarketEvent> market)
        : events(std::move(market)), g(config(), std::make_unique<VectorSource>(events), StrategySpec{})
    {}

    void run_to_end()
    {
        Timestamp ts{0};
        while (g.loop.peek(ts) == PeekStatus::Event) {
            g.loop.step();
        }
    }

    // Runs the simulator events due at the clock and returns the reports they delivered.
    std::vector<ExecutionReport> deliver()
    {
        g.loop.advance_to(g.loop.gateway().now());
        std::vector<ExecutionReport> reports = g.strategy.delivered;
        g.strategy.delivered.clear();
        return reports;
    }
};

std::vector<MarketEvent> ladder()
{
    return {add(1, 1, Side::Buy, 999'900, 100), add(2, 2, Side::Sell, 1'000'000, 100),
            add(3, 3, Side::Sell, 1'010'000, 100), add(4, 4, Side::Sell, 1'010'100, 100)};
}

Command order(OrderType type, Side side, std::int64_t price, std::uint64_t quantity)
{
    return Command{.order_type = type, .side = side, .price = Price{price}, .quantity = Quantity{quantity}};
}

TEST(GenerationTest, MarketOrdersSweepOnlyTheLevelsInsideTheirOnePercentBand)
{
    Fixture buy{ladder()};
    buy.run_to_end();
    const Applied applied = apply_command(buy.g, order(OrderType::Market, Side::Buy, 0, 300));
    ASSERT_EQ(applied.error, ErrorCode::None);
    EXPECT_EQ(applied.order_id, 1u);

    const std::vector<ExecutionReport> reports = buy.deliver();
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[0].price, Price{1'000'000});
    EXPECT_EQ(reports[0].quantity, Quantity{100});
    EXPECT_EQ(reports[1].price, Price{1'010'000});  // 1,000,000 + 1%: the band's edge
    EXPECT_EQ(reports[1].quantity, Quantity{100});
    EXPECT_EQ(reports[2].type, ReportType::Expired);  // 1,010,100 is outside the band
    EXPECT_EQ(reports[2].quantity, Quantity{100});

    Fixture sell{
        {add(1, 1, Side::Buy, 999'900, 100), add(2, 2, Side::Buy, 989'901, 100), add(3, 3, Side::Buy, 989'900, 100)}};
    sell.run_to_end();
    ASSERT_EQ(apply_command(sell.g, order(OrderType::Market, Side::Sell, 0, 300)).error, ErrorCode::None);

    const std::vector<ExecutionReport> sold = sell.deliver();
    ASSERT_EQ(sold.size(), 3u);
    EXPECT_EQ(sold[0].price, Price{999'900});
    EXPECT_EQ(sold[0].quantity, Quantity{100});
    EXPECT_EQ(sold[1].price, Price{989'901});  // 999,900 - 1%: the band's edge
    EXPECT_EQ(sold[1].quantity, Quantity{100});
    EXPECT_EQ(sold[2].type, ReportType::Expired);  // 989,900 is outside the band
    EXPECT_EQ(sold[2].quantity, Quantity{100});
}

TEST(GenerationTest, PostOnlyRestsOrIsRejectedWouldCrossAndIocTakesUpToItsPrice)
{
    Fixture f{ladder()};
    f.run_to_end();
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'900, 50)).error, ErrorCode::None);
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 1'000'000, 10)).error, ErrorCode::None);
    ASSERT_EQ(apply_command(f.g, order(OrderType::Ioc, Side::Sell, 999'900, 30)).error, ErrorCode::None);

    const std::vector<ExecutionReport> reports = f.deliver();
    ASSERT_EQ(reports.size(), 3u);
    EXPECT_EQ(reports[0].type, ReportType::Accepted);
    EXPECT_EQ(reports[1].type, ReportType::Rejected);
    EXPECT_EQ(reports[1].reason, RejectReason::WouldCross);
    EXPECT_EQ(reports[2].type, ReportType::Fill);
    EXPECT_EQ(reports[2].price, Price{999'900});
    EXPECT_EQ(reports[2].quantity, Quantity{30});
    ASSERT_EQ(f.g.execution.resting_orders().size(), 1u);
    EXPECT_EQ(f.g.execution.resting_orders()[0].ahead_quantity, 100u);  // behind the bid already at 999,900
}

TEST(GenerationTest, EveryRiskCheckAndAMissingOppositePriceRefuseWithTheirOwnCodes)
{
    Fixture f{{add(1, 1, Side::Buy, 999'900, 100)}};  // no asks
    f.run_to_end();
    EXPECT_EQ(apply_command(f.g, order(OrderType::Market, Side::Buy, 0, 10)).error, ErrorCode::NoLiquidity);
    EXPECT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 0)).error, ErrorCode::RiskInvalidOrder);
    EXPECT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 501)).error,
              ErrorCode::RiskOrderTooLarge);
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 500)).error, ErrorCode::None);
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 500)).error, ErrorCode::None);
    EXPECT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 10)).error,
              ErrorCode::RiskPositionLimit);  // 1,000 already reserved
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Sell, 1'000'100, 10)).error, ErrorCode::None);
    ASSERT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Sell, 1'000'100, 10)).error, ErrorCode::None);
    EXPECT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Sell, 1'000'100, 10)).error,
              ErrorCode::RiskTooManyOpenOrders);  // 4 open
    EXPECT_EQ(to_string(ErrorCode::RiskTooManyOpenOrders), "risk_too_many_open_orders");
    EXPECT_EQ(to_string(ErrorCode::QueueFull), "queue_full");
}

TEST(GenerationTest, AnAttachedStrategyQuotesAndRefusesManualOrdersUntilDetached)
{
    Fixture f{ladder()};
    const StrategySpec mm{.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}};
    ASSERT_EQ(apply_command(f.g, Command{.type = CommandType::Attach, .strategy = mm}).error, ErrorCode::None);
    EXPECT_TRUE(f.g.strategy.attached());
    f.run_to_end();  // quotes both sides once the ask arrives at 2
    f.deliver();
    ASSERT_EQ(f.g.execution.resting_orders().size(), 2u);
    EXPECT_EQ(apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 10)).error, ErrorCode::NotReady);
    EXPECT_EQ(apply_command(f.g, Command{.type = CommandType::Cancel, .order_id = 1}).error, ErrorCode::NotReady);

    ASSERT_EQ(apply_command(f.g, Command{.type = CommandType::Detach}).error, ErrorCode::None);
    EXPECT_FALSE(f.g.strategy.attached());
    const std::vector<ExecutionReport> released = f.deliver();  // the quotes are cancelled, not orphaned
    ASSERT_EQ(released.size(), 2u);
    EXPECT_EQ(released[0].type, ReportType::Cancelled);
    EXPECT_EQ(released[1].type, ReportType::Cancelled);
    EXPECT_TRUE(f.g.execution.resting_orders().empty());
    EXPECT_EQ(f.g.portfolio.open_orders(), 0u);
    EXPECT_EQ(f.g.portfolio.open_quantity(Side::Buy), 0u);
    EXPECT_EQ(f.g.portfolio.open_quantity(Side::Sell), 0u);
    const Applied sent = apply_command(f.g, order(OrderType::PostOnly, Side::Buy, 999'800, 10));
    ASSERT_EQ(sent.error, ErrorCode::None);
    EXPECT_EQ(sent.order_id, 3u);  // after the two quotes
    EXPECT_EQ(apply_command(f.g, Command{.type = CommandType::Cancel, .order_id = sent.order_id}).order_id,
              sent.order_id);
    const std::vector<ExecutionReport> reports = f.deliver();
    ASSERT_EQ(reports.size(), 2u);
    EXPECT_EQ(reports[0].type, ReportType::Accepted);
    EXPECT_EQ(reports[1].type, ReportType::Cancelled);
}

// Sends an IOC on the first market event and another from a timer 1 ns later, and records the session midnight and the
// book's orders as each report reaches it.
struct Probe
{
    struct Seen
    {
        OrderId order_id{0};
        Timestamp now{0};
        Timestamp midnight{0};
        std::size_t book_orders{0};

        bool operator==(const Seen&) const = default;
    };

    std::function<Timestamp()> midnight;  // the loop's session_midnight()
    std::vector<Seen> seen;

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        if (event.order_id == 1) {
            OrderId id{0};
            ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{10}, TimeInForce::Ioc, id);
            ctx.set_timer(event.timestamp + 1, 0);
        }
    }

    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        seen.push_back({.order_id = report.order_id,
                        .now = ctx.now(),
                        .midnight = midnight(),
                        .book_orders = ctx.book.order_count()});
    }

    void on_timer(StrategyContext& ctx, std::uint64_t) noexcept
    {
        OrderId id{0};
        ctx.gateway.send(Side::Buy, Price{1'000'000}, Quantity{10}, TimeInForce::Ioc, id);
    }
};

static_assert(Strategy<Probe>);

// Review gate 3: a report due at the boundary minus 1 ns runs under the old day's midnight, before the Clear; one due
// at the boundary runs after the Clear, under the new day's.
TEST(GenerationTest, AtADayBoundaryEarlierReportsRunUnderTheOldDayAndLaterOnesAfterTheClear)
{
    constexpr Timestamp boundary = 2'011;
    const std::vector<MarketEvent> events{
        add(1'000, 1, Side::Sell, 1'010'000, 100),  // order 1's report: 1,000 + 10 + 1,000; order 2's: 1 ns later
        MarketEvent{.timestamp = boundary, .symbol = kSymbol, .type = MarketEventType::Clear},
        add(boundary + 5, 2, Side::Sell, 1'010'000, 100)};
    VectorSource source{events};
    MarketBook book{kSymbol, 16};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, config().limits, FeeSchedule{}};
    Probe probe;
    EventLoop<Probe> loop{source, book, execution, portfolio, probe, {.order_latency = 10, .report_latency = 1'000}};
    probe.midnight = [&loop] { return loop.session_midnight(); };

    Timestamp ts{0};
    while (loop.peek(ts) == PeekStatus::Event && server::step(loop).ok) {  // not the test helper step(n)
    }
    loop.finish();
    EXPECT_EQ(probe.seen,
              (std::vector<Probe::Seen>{{.order_id = 1, .now = boundary - 1, .midnight = 0, .book_orders = 1},
                                        {.order_id = 2, .now = boundary, .midnight = boundary, .book_orders = 0}}));
}

}  // namespace
}  // namespace simulator::server::test
