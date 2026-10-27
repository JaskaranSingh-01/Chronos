#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chronos/server/generation.hpp"
#include "chronos/server/itch_input.hpp"
#include "simulator/market/vector_source.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;

// A generation over `market`, which it keeps alive.
struct Fixture
{
    std::vector<MarketEvent> events;
    Generation g;

    explicit Fixture(std::vector<MarketEvent> market)
        : events(std::move(market)), g(config(), config().defaults, std::make_unique<VectorSource>(events))
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

// --book reaches every generation's book; the backends give the same book.
TEST(GenerationTest, ItsBookTakesTheSessionsBackend)
{
    SessionConfig ladder_config = config();
    ladder_config.book = BookBackend::Ladder;
    const std::vector<MarketEvent> events = ladder();
    Generation list{config(), config().defaults, std::make_unique<VectorSource>(events)};
    Generation with_ladder{ladder_config, ladder_config.defaults, std::make_unique<VectorSource>(events)};
    for (Generation* g : {&list, &with_ladder}) {
        Timestamp ts{0};
        while (g->loop.peek(ts) == PeekStatus::Event) {
            ASSERT_TRUE(g->loop.step().ok);
        }
    }
    EXPECT_EQ(list.book.storage().backend, BookBackend::List);
    EXPECT_EQ(with_ladder.book.storage().backend, BookBackend::Ladder);
    EXPECT_EQ(with_ladder.book.digest(), list.book.digest());
    EXPECT_EQ(with_ladder.book.order_count(), 4u);
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

// A report due at the boundary minus 1 ns runs under the old day's midnight, before the Clear; one due
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
    Portfolio portfolio{kEquity, config().defaults.limits, FeeSchedule{}};
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

// The Portfolio and the strategy live through the night: Friday's position, cash, fees and marks are Monday's at the
// Clear, and the market maker attached on Friday quotes again on Monday. Only the resting orders expire.
TEST(GenerationTest, ThePortfolioAndTheStrategyCarryOvernight)
{
    const ScratchDir dir{"overnight"};
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", acme_day(4).bytes());
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", acme_day(4, 1'000).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    RunSpec spec = config().defaults;
    spec.ticker = ticker("ACME");
    spec.days = {.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2};
    spec.fees = {.taker_fee_per_share = 3};
    Generation g{config(), spec, chain_factory(library, kEquity, false)(spec.ticker, spec.days)};
    Timestamp ts{0};
    for (int i = 0; i < 3; ++i) {  // Friday's trading state, bid and offer
        ASSERT_EQ(g.loop.peek(ts), PeekStatus::Event);
        server::step(g.loop);
    }
    ASSERT_EQ(apply_command(g, order(OrderType::Ioc, Side::Buy, 100'100, 100)).error, ErrorCode::None);
    ASSERT_EQ(apply_command(
                  g,
                  Command{.type = CommandType::Attach,
                          .strategy = {.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}}})
                  .error,
              ErrorCode::None);
    while (g.loop.peek(ts) == PeekStatus::Event && g.loop.pending()->type != MarketEventType::Clear) {
        server::step(g.loop);
    }
    g.loop.advance_to(ts - 1);  // the end of Friday
    const Portfolio& p = g.portfolio;
    const std::array<std::int64_t, 4> friday{p.position(), p.cash(), p.fees(), p.pnl()};
    EXPECT_EQ(friday[0], 100);
    EXPECT_GT(friday[2], 0);  // the IOC's taker fee
    EXPECT_FALSE(g.execution.resting_orders().empty());

    server::step(g.loop);  // the Clear
    g.loop.advance_to(ts);  // its Expired reports
    EXPECT_EQ((std::array<std::int64_t, 4>{p.position(), p.cash(), p.fees(), p.pnl()}), friday);
    EXPECT_TRUE(g.execution.resting_orders().empty());
    EXPECT_EQ(p.open_orders(), 0u);
    EXPECT_TRUE(g.strategy.attached());
    EXPECT_EQ(g.loop.session_midnight(), session_midnight_ns(2019y / 12 / 30, -5h));

    while (g.loop.peek(ts) == PeekStatus::Event && server::step(g.loop).ok) {
    }
    EXPECT_FALSE(g.execution.resting_orders().empty());  // quoting on Monday's book
}

// The reports a generation's strategy was delivered, as "accepted 1 @<exchange ns>".
std::vector<std::string> delivered(const Generation& g)
{
    std::vector<std::string> lines;
    for (const ExecutionReport& r : g.strategy.delivered) {
        lines.push_back(report_name(r.type) + " " + std::to_string(r.order_id) + " @" +
                        std::to_string(r.exchange_time));
    }
    return lines;
}

// Steps a generation to the end of its stream, then ends it.
void run_to_end(Generation& g)
{
    Timestamp ts{0};
    while (g.loop.peek(ts) == PeekStatus::Event && server::step(g.loop).ok) {
    }
    finish(g);
}

// Under a Regular window the market maker trades only inside each day's feed session. Friday has no
// 'Q' or 'M', so it takes the flagged 09:30-16:00 fallback; Monday's 'M' closes it at 13:00. Outside the window the
// strategy sees no market event and its gateway refuses it; each close cancels its quotes; the window moves to Monday
// at the Clear. A Full-day run of the same spec quotes on Friday's 09:00 book.
TEST(GenerationTest, ARegularWindowTradesOnlyInsideEachDaysFeedSession)
{
    constexpr std::uint64_t hour = 3'600 * kSecond;
    itch_test::ItchBuilder friday;
    friday.system_event(0, 3 * hour, 'O')
        .stock_directory(1, 3 * hour, "ACME")
        .trading_action(1, 3 * hour, "ACME", 'T')
        .system_event(0, 4 * hour, 'S')
        .add_order(1, 9 * hour, 1, 'B', 1'000, "ACME", 100'000)
        .add_order(1, 9 * hour, 2, 'S', 1'000, "ACME", 100'100)
        .add_order(1, 10 * hour, 3, 'B', 100, "ACME", 99'900)     // the first event inside the fallback session
        .add_order(1, 17 * hour, 4, 'B', 100, "ACME", 100'050)    // a better bid, after the close
        .executed(1, 17 * hour + kSecond, 3, 100, 1);             // a trade through our bid, after the close
    itch_test::ItchBuilder monday;
    monday.system_event(0, 3 * hour, 'O')
        .stock_directory(1, 3 * hour, "ACME")
        .trading_action(1, 3 * hour, "ACME", 'T')
        .system_event(0, 4 * hour, 'S')
        .system_event(0, kNineThirty, 'Q')
        .add_order(1, kNineThirty + kSecond, 1, 'B', 1'000, "ACME", 100'000)
        .add_order(1, kNineThirty + kSecond, 2, 'S', 1'000, "ACME", 100'100)
        .system_event(0, 13 * hour, 'M')
        .add_order(1, 14 * hour, 3, 'B', 100, "ACME", 100'050);
    const ScratchDir dir{"window"};
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", friday.bytes());
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", monday.bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    ASSERT_EQ(library.find(2019y / 12 / 27)->session.source, SessionSource::Fallback);
    RunSpec spec = config({.order_latency = kMs, .report_latency = kMs}).defaults;
    spec.ticker = ticker("ACME");
    spec.days = {.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2};
    spec.window = TradingWindow::Regular;
    spec.strategy = {.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}};
    const SourceFactory chain = chain_factory(library, kEquity, false);

    Generation g{config(), spec, chain(spec.ticker, spec.days)};
    run_to_end(g);
    const auto at = [](Timestamp ts) { return " @" + std::to_string(ts); };
    const Timestamp fri = session_midnight_ns(2019y / 12 / 27, -5h);
    const Timestamp mon = session_midnight_ns(2019y / 12 / 30, -5h);
    EXPECT_EQ(
        delivered(g),
        (std::vector<std::string>{
            "accepted 1" + at(fri + 10 * hour + kMs), "accepted 2" + at(fri + 10 * hour + kMs),
            "cancelled 1" + at(fri + 16 * hour + kMs), "cancelled 2" + at(fri + 16 * hour + kMs),
            "accepted 3" + at(mon + kNineThirty + kSecond + kMs), "accepted 4" + at(mon + kNineThirty + kSecond + kMs),
            "cancelled 3" + at(mon + 13 * hour + kMs), "cancelled 4" + at(mon + 13 * hour + kMs)}));
    EXPECT_TRUE(g.execution.resting_orders().empty());
    EXPECT_EQ(g.portfolio.open_orders(), 0u);
    EXPECT_EQ(g.execution.statistics().maker_fills, 0u);  // Friday's 17:00:01 trade-through finds no quote

    spec.window = TradingWindow::FullDay;
    Generation full{config(), spec, chain(spec.ticker, spec.days)};
    run_to_end(full);
    const std::vector<std::string> lines = delivered(full);
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines[0], "accepted 1" + at(fri + 9 * hour + kMs));
    for (const std::string& line : lines) {
        EXPECT_EQ(line.find(at(fri + 16 * hour + kMs)), std::string::npos) << line;  // a full day never closes mid-day
    }
    EXPECT_TRUE(std::ranges::any_of(lines, [&](const std::string& line) {
        return line.starts_with("fill ") && line.ends_with(at(fri + 17 * hour + kSecond));  // the same trade fills it
    }));
}

// A quote still in flight at the close is cancelled when its Accepted arrives; one that arrived within a report latency
// before it rested at the timer, which cancelled it, so its late Accepted cancels nothing again. Manual orders have no
// window, before the session or after a detach that follows the close.
TEST(GenerationTest, AQuoteInFlightAtTheCloseIsCancelledOnItsAcceptedAndManualOrdersHaveNoWindow)
{
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 30, -5h);
    const Timestamp close = midnight + RTH_CLOSE_NS;  // a VectorSource's days take the 09:30-16:00 fallback
    const std::vector<MarketEvent> events{add(midnight + 8 * 3'600 * kSecond, 1, Side::Buy, 999'000, 100),
                                          add(close - kMs / 2, 2, Side::Buy, 1'000'000, 100),
                                          add(close - kMs / 2, 3, Side::Sell, 1'000'100, 100),
                                          add(close + kSecond, 4, Side::Buy, 1'000'050, 100)};
    RunSpec spec = config({.order_latency = kMs, .report_latency = kMs}).defaults;
    spec.days = {.day = {2019y / 12 / 30}, .count = 1};
    spec.window = TradingWindow::Regular;
    spec.strategy = {.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}};
    Generation g{config(), spec, std::make_unique<VectorSource>(events)};
    Timestamp ts{0};
    while (g.loop.peek(ts) == PeekStatus::Event && server::step(g.loop).ok) {
    }
    g.loop.advance_to(ts);
    const auto at = [](Timestamp t) { return " @" + std::to_string(t); };
    EXPECT_EQ(delivered(g),
              (std::vector<std::string>{"accepted 1" + at(close + kMs / 2), "accepted 2" + at(close + kMs / 2),
                                        "cancelled 1" + at(close + kMs / 2 + 2 * kMs),
                                        "cancelled 2" + at(close + kMs / 2 + 2 * kMs)}));
    EXPECT_TRUE(g.execution.resting_orders().empty());
    OrderId id{0};
    EXPECT_EQ(g.loop.gateway().send(Side::Buy, Price{999'000}, Quantity{10}, TimeInForce::Day, id), RiskCheck::Closed);
    ASSERT_EQ(apply_command(g, Command{.type = CommandType::Detach}).error, ErrorCode::None);
    EXPECT_EQ(apply_command(g, order(OrderType::PostOnly, Side::Buy, 999'000, 10)).error, ErrorCode::None);

    const std::vector<MarketEvent> early{add(midnight + 8 * 3'600 * kSecond, 1, Side::Buy, 999'000, 100),
                                         add(close - 3 * kMs / 2, 2, Side::Buy, 1'000'000, 100),
                                         add(close - 3 * kMs / 2, 3, Side::Sell, 1'000'100, 100),
                                         add(close + kSecond, 4, Side::Buy, 1'000'050, 100)};
    Generation resting{config(), spec, std::make_unique<VectorSource>(early)};  // the quotes arrive at close - 0.5 ms
    while (resting.loop.peek(ts) == PeekStatus::Event && server::step(resting.loop).ok) {
    }
    resting.loop.advance_to(ts);
    EXPECT_EQ(delivered(resting),
              (std::vector<std::string>{"accepted 1" + at(close - kMs / 2), "accepted 2" + at(close - kMs / 2),
                                        "cancelled 1" + at(close + kMs), "cancelled 2" + at(close + kMs)}));
    EXPECT_EQ(resting.execution.statistics().cancel_rejects, 0u);

    spec.strategy = {};
    Generation manual{config(), spec, std::make_unique<VectorSource>(events)};
    ASSERT_EQ(manual.loop.peek(ts), PeekStatus::Event);
    server::step(manual.loop);  // 08:00, before the session
    EXPECT_EQ(apply_command(manual, order(OrderType::PostOnly, Side::Buy, 999'000, 10)).error, ErrorCode::None);
}

}  // namespace
}  // namespace simulator::server::test
