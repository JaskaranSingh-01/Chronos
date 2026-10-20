#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "chronos/server/backtest.hpp"
#include "chronos/server/itch_input.hpp"
#include "simulator/cli/replay_args.hpp"
#include "simulator/itch/itch_source.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kHour = 3'600 * kSecond;
constexpr DayList kMonday{.day = {2019y / 12 / 30}, .count = 1};
constexpr DayList kFridayAndMonday{.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2};

// Day files in a scratch directory, and the library over them.
struct Lab
{
    ScratchDir dir;
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};

    explicit Lab(const std::string& name)
        : dir(name)
    {}

    void write(const char* file, std::span<const std::byte> bytes)
    {
        write_file(dir.path / file, bytes);
    }

    std::vector<DayHandle> days()
    {
        return library.scan();
    }
};

// The book capacity and scales `simulator backtest` uses by default.
SessionConfig lab_config()
{
    return SessionConfig{.session_id = "test", .instrument = kEquity};
}

// The CLI's settings for `kind` on ACME over `days`.
RunSpec acme(SessionStrategyKind kind, const DayList& days, TradingWindow window = TradingWindow::Regular)
{
    RunSpec spec = default_spec(kind);
    spec.ticker = ticker("ACME");
    spec.days = days;
    spec.window = window;
    return spec;
}

RunResult run(const RunSpec& spec, std::span<const DayHandle> days, bool hash_as_read = false,
              const SessionConfig& config = lab_config())
{
    const std::atomic<bool> cancel{false};
    std::atomic<std::uint64_t> bytes_read{0};
    return backtest(config, spec, days, hash_as_read, cancel, bytes_read);
}

// The book, execution and portfolio digests as 16-digit hex, space-separated.
std::string hex(const Digests& d)
{
    char text[64];
    std::snprintf(text, sizeof text, "%016" PRIx64 " %016" PRIx64 " %016" PRIx64, d.book, d.execution, d.portfolio);
    return text;
}

// A one-day, Full-day run with the CLI's defaults is `simulator backtest`: the EventLoop app/main.cpp builds from
// BacktestArgs{} over the same bytes gives the same PnL, fills, fill attribution and book, for both strategies.
TEST(BacktestTest, AOneDayFullDayRunEqualsTheHeadlessBacktestForBothStrategies)
{
    Lab lab{"backtest-headless"};
    const itch_test::ItchBuilder monday = lab_day(2);
    lab.write("12302019.NASDAQ_ITCH50.bin", monday.bytes());
    const std::vector<DayHandle> days = lab.days();
    const BacktestArgs b{};
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 30, -5h);
    for (const bool mm : {true, false}) {
        SCOPED_TRACE(mm ? "market maker" : "imbalance");
        const RunResult r = run(acme(mm ? SessionStrategyKind::MarketMaker : SessionStrategyKind::ImbalanceTaker,
                                     kMonday, TradingWindow::FullDay),
                                days);

        InstrumentRegistry registry{1};
        ItchSource source{monday.bytes(),
                          ItchSourceConfig{.ticker = "ACME", .session_date = 2019y / 12 / 30, .utc_offset = -5h},
                          registry};
        MarketBook book{0, std::size_t{1} << 20};
        ExecutionSimulator execution{midnight};
        Portfolio portfolio{
            source.instrument(),
            {.max_position = b.max_position,
             .max_order_quantity = Quantity{mm ? b.quote_size : b.clip},
             .max_open_orders = default_max_open_orders(mm ? StrategyKind::MarketMaker : StrategyKind::Imbalance)},
            {.maker_fee_per_share = b.maker_fee_ticks,
             .taker_fee_per_share = static_cast<std::int64_t>(b.taker_fee_ticks)}};
        const LatencyConfig latency{.order_latency = static_cast<Timestamp>(b.order_latency_ns),
                                    .report_latency = static_cast<Timestamp>(b.report_latency_ns)};
        const ReplayOptions options{.session_midnight = midnight};
        LoopReport report;
        if (mm) {
            MarketMaker strategy{{.quote_size = b.quote_size,
                                  .max_position = b.max_position,
                                  .requote_cooldown_ns = static_cast<Timestamp>(b.requote_ms) * 1'000'000}};
            report = EventLoop<MarketMaker>{source, book, execution, portfolio, strategy, latency, options}.run();
        } else {
            ImbalanceTaker strategy{{.threshold = b.threshold,
                                     .clip = b.clip,
                                     .max_position = b.max_position,
                                     .cooldown_ns = static_cast<Timestamp>(b.cooldown_ms) * 1'000'000}};
            report = EventLoop<ImbalanceTaker>{source, book, execution, portfolio, strategy, latency, options}.run();
        }

        EXPECT_GT(mm ? r.maker_fills : r.taker_fills, 0u);
        EXPECT_EQ(r.pnl, portfolio.pnl());
        EXPECT_EQ(r.fees, portfolio.fees());
        EXPECT_EQ(r.position, portfolio.position());
        EXPECT_EQ(r.max_drawdown, portfolio.max_drawdown());
        EXPECT_EQ(r.max_abs_position, portfolio.max_abs_position());
        EXPECT_EQ(r.maker_fills, execution.statistics().maker_fills);
        EXPECT_EQ(r.taker_fills, execution.statistics().taker_fills);
        EXPECT_EQ(r.fills_by, execution.statistics().fills_by);
        EXPECT_EQ(r.anomalies_by, execution.statistics().anomalies_by);
        EXPECT_EQ(r.digests.book, book.digest());
        EXPECT_EQ(r.events, report.replay.events);
        EXPECT_EQ(r.sim_events, report.sim_events);
    }
}

// The fixture runs' digests, fixed: every compiler and build type of the CI matrix must reproduce them (spec §8).
TEST(BacktestTest, TheFixtureRunsDigestsAreGolden)
{
    Lab lab{"backtest-golden"};
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
    const std::vector<DayHandle> days = lab.days();
    const std::span<const DayHandle> monday = std::span{days}.last(1);
    EXPECT_EQ(hex(run(acme(SessionStrategyKind::MarketMaker, kMonday), monday).digests),
              "c7eb451b0b743f7d 6ae9bf90f1f85c85 0df0811549464c97");
    EXPECT_EQ(hex(run(acme(SessionStrategyKind::ImbalanceTaker, kMonday), monday).digests),
              "c7eb451b0b743f7d 84d6e3d50e269aaf 031bb6d84af959c8");
    EXPECT_EQ(hex(run(acme(SessionStrategyKind::MarketMaker, kFridayAndMonday), days).digests),
              "c7eb451b0b743f7d d623951ea5a8287b 93933452927d752d");
    EXPECT_EQ(hex(run(acme(SessionStrategyKind::ImbalanceTaker, kFridayAndMonday), days).digests),
              "c7eb451b0b743f7d 0a99d7d61f572c28 d71a2657f86e95ec");
}

// by_day and by_hour each add up to the PnL; the curve has one sample per exchange second with a market event, the
// last taken at the end; Max drawdown is the Portfolio's, as stepping the chain straight through gives it.
TEST(BacktestTest, TheDaysHoursAndCurveAddUpToThePnlWithOneSamplePerEventSecond)
{
    Lab lab{"backtest-curve"};
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
    const std::vector<DayHandle> days = lab.days();
    const RunSpec spec = acme(SessionStrategyKind::MarketMaker, kFridayAndMonday);
    const RunResult r = run(spec, days);

    ASSERT_EQ(r.by_day.size(), 2u);
    EXPECT_EQ(r.by_day[0].date, 2019y / 12 / 27);
    EXPECT_EQ(r.by_day[1].date, 2019y / 12 / 30);
    EXPECT_EQ(r.by_day[0].pnl + r.by_day[1].pnl, r.pnl);
    EXPECT_EQ(r.by_day[0].fills + r.by_day[1].fills, r.maker_fills + r.taker_fills);
    EXPECT_GT(r.by_day[0].fills, 0u);
    EXPECT_GT(r.by_day[1].fills, 0u);
    std::int64_t hours = 0;
    for (const auto& [hour, pnl] : r.by_hour) {
        hours += pnl;
    }
    EXPECT_EQ(hours, r.pnl);
    ASSERT_FALSE(r.equity.empty());
    EXPECT_EQ(r.equity.back().pnl, r.pnl);
    EXPECT_EQ(r.equity.back().position, r.position);
    ASSERT_EQ(r.fills.size(), r.maker_fills + r.taker_fills);
    EXPECT_EQ(r.fills.back().position, r.position);
    EXPECT_FALSE(r.truncated);

    std::set<std::int64_t> seconds;
    DaySequenceSource chain{days, "ACME", kEquity, false, false};
    MarketEvent event;
    while (chain.next(event) == SourceResult::Event) {
        if (event.type != MarketEventType::Clear) {
            seconds.insert(event.timestamp / static_cast<std::int64_t>(kSecond));
        }
    }
    std::vector<std::int64_t> sampled;
    for (const Sample& s : r.equity) {
        sampled.push_back(s.second);
    }
    EXPECT_EQ(sampled, std::vector<std::int64_t>(seconds.begin(), seconds.end()));

    Generation straight{lab_config(), spec, std::make_unique<DaySequenceSource>(days, "ACME", kEquity, false, false)};
    Timestamp ts{0};
    while (straight.loop.peek(ts) == PeekStatus::Event && server::step(straight.loop).ok) {
    }
    finish(straight);
    EXPECT_EQ(r.max_drawdown, straight.portfolio.max_drawdown());
    EXPECT_EQ(r.digests, digests(straight));
    EXPECT_EQ(r.fills_by, straight.execution.statistics().fills_by);  // phases under each day's own midnight
    EXPECT_LT(r.by_hour.rbegin()->first, 24);
}

// A run lists its first 50,000 fills, and counts them all: here the imbalance taker buys 1 share at every event.
TEST(BacktestTest, TheFillListStopsAtItsCapButTheCountersCountEveryFill)
{
    itch_test::ItchBuilder itch;
    itch.system_event(0, 3 * kHour, 'O')
        .stock_directory(1, 3 * kHour, "ACME")
        .trading_action(1, 3 * kHour, "ACME", 'T')
        .add_order(1, 10 * kHour, 1, 'B', 10'000, "ACME", 100'000)
        .add_order(1, 10 * kHour, 2, 'S', 100, "ACME", 100'100);
    for (std::uint64_t i = 1; i <= max_fill_rows + 10; ++i) {
        itch.add_order(1, 10 * kHour + i * kMs, 2 + i, 'B', 100, "ACME", 99'000);
    }
    Lab lab{"backtest-cap"};
    lab.write("12302019.NASDAQ_ITCH50.bin", itch.bytes());
    RunSpec spec = acme(SessionStrategyKind::ImbalanceTaker, kMonday, TradingWindow::FullDay);
    spec.strategy.imbalance = {.threshold = 3.0, .clip = 1, .max_position = 1'000'000'000, .cooldown_ns = 0};
    spec.limits = default_limits(spec.strategy);
    const RunResult r = run(spec, lab.days());

    constexpr std::uint64_t fills = max_fill_rows + 11;  // one at the offer's arrival, then one an event
    EXPECT_EQ(r.taker_fills, fills);
    EXPECT_EQ(r.filled_shares, fills);
    EXPECT_EQ(r.by_day.at(0).fills, fills);
    EXPECT_EQ(r.position, static_cast<std::int64_t>(fills));
    ASSERT_EQ(r.fills.size(), max_fill_rows);
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.fills.back().position, static_cast<std::int64_t>(max_fill_rows));
    EXPECT_EQ(r.fills.back().liquidity, Liquidity::Taker);
    EXPECT_DOUBLE_EQ(r.fill_rate, 1.0);
}

// A cancel is seen before every market event. A failed run names its reason, and its day for a source error, and keeps
// what it had: the days, the curve and the fills so far.
TEST(BacktestTest, ACancelLandsBeforeTheNextEventAndAFailedRunKeepsItsAggregates)
{
    Lab lab{"backtest-failures"};
    const itch_test::ItchBuilder monday = lab_day(2);
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", monday.bytes().first(monday.bytes().size() - 5));  // 'M' cut off
    const std::vector<DayHandle> days = lab.days();
    const RunSpec spec = acme(SessionStrategyKind::MarketMaker, kFridayAndMonday);

    const std::atomic<bool> cancel{true};
    std::atomic<std::uint64_t> bytes_read{0};
    const RunResult cancelled = backtest(lab_config(), spec, days, false, cancel, bytes_read);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_TRUE(cancelled.partial);
    EXPECT_EQ(cancelled.reason, "");
    EXPECT_EQ(cancelled.events, 0u);
    EXPECT_TRUE(cancelled.equity.empty());

    const RunResult failed = run(spec, days);
    EXPECT_FALSE(failed.cancelled);
    EXPECT_TRUE(failed.partial);
    EXPECT_EQ(failed.reason, "source_error on 2019-12-30: TruncatedMessage");
    ASSERT_EQ(failed.by_day.size(), 2u);
    EXPECT_EQ(
        failed.by_day[0],
        run(acme(SessionStrategyKind::MarketMaker, {.day = {2019y / 12 / 27}, .count = 1}), std::span{days}.first(1))
            .by_day[0]);
    EXPECT_GT(failed.by_day[1].fills, 0u);
    EXPECT_FALSE(failed.equity.empty());

    itch_test::ItchBuilder backwards;
    backwards.system_event(0, 3 * kHour, 'O')
        .stock_directory(1, 3 * kHour, "ACME")
        .add_order(1, 10 * kHour, 1, 'B', 100, "ACME", 100'000)
        .add_order(1, 10 * kHour - 1, 2, 'S', 100, "ACME", 100'100);
    itch_test::ItchBuilder crossed;
    crossed.system_event(0, 3 * kHour, 'O')
        .stock_directory(1, 3 * kHour, "ACME")
        .add_order(1, 10 * kHour, 1, 'S', 100, "ACME", 100'100)
        .add_order(1, 10 * kHour, 2, 'B', 100, "ACME", 100'200);
    Lab more{"backtest-invalid"};
    more.write("12302019.NASDAQ_ITCH50.bin", backwards.bytes());
    more.write("12312019.NASDAQ_ITCH50.bin", crossed.bytes());
    const std::vector<DayHandle> invalid = more.days();
    EXPECT_EQ(run(acme(SessionStrategyKind::MarketMaker, kMonday), std::span{invalid}.first(1)).reason,
              "out_of_order_timestamp");
    SessionConfig checked = lab_config();
    checked.check_invariants_every = 1;
    const RunResult crossing = run(acme(SessionStrategyKind::MarketMaker, {.day = {2019y / 12 / 31}, .count = 1}),
                                   std::span{invalid}.last(1), false, checked);
    EXPECT_EQ(crossing.reason, "invariant_violation");
    EXPECT_TRUE(crossing.partial);
    EXPECT_EQ(crossing.digests, Digests{});
}

// The same identity run twice gives the same results, timings aside. Hash as read changes nothing but adds each day's
// re-hash, which is the hash the library recorded.
TEST(BacktestTest, TheSameRunTwiceGivesTheSameResultsAndHashAsReadAddsTheDaysHashes)
{
    Lab lab{"backtest-twice"};
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
    const std::vector<DayHandle> days = lab.days();
    const RunSpec spec = acme(SessionStrategyKind::ImbalanceTaker, kFridayAndMonday);
    RunResult first = run(spec, days);
    RunResult second = run(spec, days, true);
    EXPECT_EQ(second.hashes, (std::vector<std::string>{days[0]->hash, days[1]->hash}));
    second.hashes.clear();
    first.wall_ms = second.wall_ms;
    first.cpu_ms = second.cpu_ms;
    EXPECT_EQ(first, second);
    EXPECT_GT(first.taker_fills, 0u);
}

// Review gate 4, in backtest(): Monday's input ends at 09:30:02 with the market maker's bid half filled and its offer
// resting. The 16:00 close cancels both with no market event after them, so 50 of the 200 shares quoted filled.
TEST(BacktestTest, TheLastDaysCloseCancelsTheQuotesWhenItsInputEndsFirst)
{
    itch_test::ItchBuilder itch;
    itch.system_event(0, 3 * kHour, 'O')
        .stock_directory(1, 3 * kHour, "ACME")
        .trading_action(1, 3 * kHour, "ACME", 'T')
        .system_event(0, kNineThirty, 'Q')
        .add_order(1, kNineThirty, 1, 'B', 1'000, "ACME", 100'000)
        .add_order(1, kNineThirty, 2, 'S', 1'000, "ACME", 100'100)
        .add_order(1, kNineThirty + kSecond, 3, 'B', 100, "ACME", 100'000)  // behind the market maker's bid
        .executed(1, kNineThirty + 2 * kSecond, 3, 50, 1)                    // rule (a): fills the bid 50
        .system_event(0, 16 * kHour, 'M');
    Lab lab{"backtest-close"};
    lab.write("12302019.NASDAQ_ITCH50.bin", itch.bytes());
    const RunResult r = run(acme(SessionStrategyKind::MarketMaker, kMonday), lab.days());

    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].rule, FillCause::RuleAExecute);
    EXPECT_EQ(r.fills[0].quantity, Quantity{50});
    EXPECT_DOUBLE_EQ(r.fill_rate, 0.25);
    EXPECT_FALSE(r.partial);
}

}  // namespace
}  // namespace simulator::server::test
