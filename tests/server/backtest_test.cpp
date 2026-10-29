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
#include "simulator/strategies/imbalance_taker.hpp"
#include "simulator/strategies/market_maker.hpp"
#include "simulator/strategies/orb.hpp"
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

// The CLI's settings for the registered `strategy` on ACME over `days`.
RunSpec acme(std::string_view strategy, const DayList& days, TradingWindow window = TradingWindow::Regular)
{
    RunSpec spec = default_spec(strategy_spec(strategy));
    spec.ticker = ticker("ACME");
    spec.days = days;
    spec.window = window;
    return spec;
}

// The strategy's own stat `name` in `r`, or -1.
double stat(const RunResult& r, std::string_view name)
{
    for (const Stat& s : r.stats) {
        if (s.name == name) {
            return s.value;
        }
    }
    return -1;
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
        const RunResult r = run(acme(mm ? "market_maker" : "imbalance", kMonday, TradingWindow::FullDay), days);

        InstrumentRegistry registry{1};
        ItchSource source{monday.bytes(),
                          ItchSourceConfig{.ticker = "ACME", .session_date = 2019y / 12 / 30, .utc_offset = -5h},
                          registry};
        MarketBook book{0, std::size_t{1} << 20};
        ExecutionSimulator execution{midnight};
        // The strategies' own limits at their default params: 500 shares, 100 a quote or clip, two quotes or one IOC.
        Portfolio portfolio{source.instrument(),
                            {.max_position = 500, .max_order_quantity = Quantity{100}, .max_open_orders = mm ? 2u : 1u},
                            {.maker_fee_per_share = b.maker_fee_ticks,
                             .taker_fee_per_share = static_cast<std::int64_t>(b.taker_fee_ticks)}};
        const LatencyConfig latency{.order_latency = static_cast<Timestamp>(b.order_latency_ns),
                                    .report_latency = static_cast<Timestamp>(b.report_latency_ns)};
        const ReplayOptions options{.session_midnight = midnight};
        LoopReport report;
        std::uint64_t strategy_fills = 0;
        if (mm) {
            MarketMaker strategy{MarketMakerParams{}};
            report = EventLoop<MarketMaker>{source, book, execution, portfolio, strategy, latency, options}.run();
            strategy_fills = strategy.stats().fills;
        } else {
            ImbalanceTaker strategy{ImbalanceTakerParams{}};
            report = EventLoop<ImbalanceTaker>{source, book, execution, portfolio, strategy, latency, options}.run();
            strategy_fills = strategy.stats().fills;
        }

        EXPECT_GT(mm ? r.maker_fills : r.taker_fills, 0u);
        EXPECT_EQ(stat(r, "Fills"), static_cast<double>(strategy_fills));  // its own stats, through the registry
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

// An ORB run is `simulator backtest --strategy orb` too, on a day built for it: a range from executions in [09:30,
// 09:31), a long on a trade above it and its stop on a trade below, a short on a later trade, flattened at 15:55 by the
// timer the end of the stream still fires.
TEST(BacktestTest, AnOrbRunEqualsTheHeadlessBacktest)
{
    constexpr std::uint64_t hour = 3'600 * kSecond;
    constexpr std::uint64_t minute = 60 * kSecond;
    itch_test::ItchBuilder day;
    day.system_event(0, 3 * hour, 'O')
        .stock_directory(1, 3 * hour, "ACME")
        .trading_action(1, 3 * hour, "ACME", 'T')
        .system_event(0, 4 * hour, 'S')
        .add_order(1, 9 * hour, 1, 'B', 5'000, "ACME", 100'000)  // 10.00 and 10.01, deep enough for every IOC
        .add_order(1, 9 * hour, 2, 'S', 5'000, "ACME", 100'100)
        .system_event(0, kNineThirty, 'Q')
        .executed(1, kNineThirty + 10 * kSecond, 1, 100, 1)  // the range: 10.00 to 10.01
        .executed(1, kNineThirty + 20 * kSecond, 2, 100, 2)
        .add_order(1, kNineThirty + 2 * minute, 3, 'S', 100, "ACME", 100'300)
        .executed(1, kNineThirty + 2 * minute + kSecond, 3, 100, 3)  // 10.03: the long
        .add_order(1, kNineThirty + 3 * minute, 4, 'B', 100, "ACME", 99'800)
        .executed(1, kNineThirty + 3 * minute + kSecond, 4, 100, 4)  // 9.98: its stop
        .add_order(1, kNineThirty + 4 * minute, 5, 'B', 100, "ACME", 99'700)
        .executed(1, kNineThirty + 4 * minute + kSecond, 5, 100, 5)  // 9.97, a later trade: the short
        .system_event(0, 16 * hour, 'M');
    Lab lab{"backtest-orb"};
    lab.write("12302019.NASDAQ_ITCH50.bin", day.bytes());
    const std::vector<DayHandle> days = lab.days();
    RunSpec spec = acme("orb", kMonday, TradingWindow::FullDay);
    spec.strategy = strategy_spec("orb", std::vector<Param>{{"range_minutes", 1}});
    const RunResult r = run(spec, days);

    const BacktestArgs b{};
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 30, -5h);
    InstrumentRegistry registry{1};
    ItchSource source{day.bytes(),
                      ItchSourceConfig{.ticker = "ACME", .session_date = 2019y / 12 / 30, .utc_offset = -5h}, registry};
    MarketBook book{0, std::size_t{1} << 20};
    ExecutionSimulator execution{midnight};
    Portfolio portfolio{
        source.instrument(), {.max_position = 500, .max_order_quantity = Quantity{100}, .max_open_orders = 1}, {}};
    const DaySession session{.open = midnight + RTH_OPEN_NS, .close = midnight + RTH_CLOSE_NS};
    Orb strategy{{.range_minutes = 1, .clip = 100}, {&session, 1}};
    EventLoop<Orb>{source,
                   book,
                   execution,
                   portfolio,
                   strategy,
                   {.order_latency = static_cast<Timestamp>(b.order_latency_ns),
                    .report_latency = static_cast<Timestamp>(b.report_latency_ns)},
                   {.session_midnight = midnight}}
        .run();

    EXPECT_EQ(stat(r, "Long entries"), 1);
    EXPECT_EQ(stat(r, "Stops"), 1);
    EXPECT_EQ(stat(r, "Short entries"), 1);
    EXPECT_EQ(stat(r, "Flattens"), 1);
    EXPECT_EQ(stat(r, "Range high"), static_cast<double>(strategy.stats().range_high.ticks));
    EXPECT_EQ(strategy.stats().long_entries, 1u);
    EXPECT_EQ(strategy.stats().stops, 1u);
    EXPECT_EQ(strategy.stats().short_entries, 1u);
    EXPECT_EQ(strategy.stats().flattens, 1u);
    EXPECT_EQ(r.position, 0);
    EXPECT_EQ(r.position, portfolio.position());
    EXPECT_EQ(r.pnl, portfolio.pnl());
    EXPECT_EQ(r.taker_fills, execution.statistics().taker_fills);
    EXPECT_EQ(r.taker_fills, 4u);
    EXPECT_EQ(r.pnl, -20'000);  // two round trips of 100 shares, each across the cent spread: -$2.00

    // The Lab's default window, Regular: its timer forwarded and its gateway open at 15:55, the same run.
    RunSpec regular = spec;
    regular.window = TradingWindow::Regular;
    const RunResult again = run(regular, days);
    EXPECT_EQ(again.stats, r.stats);
    EXPECT_EQ(again.pnl, r.pnl);
}

// The fixture runs' digests, fixed: every compiler and build type of the CI matrix must reproduce them.
TEST(BacktestTest, TheFixtureRunsDigestsAreGolden)
{
    Lab lab{"backtest-golden"};
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
    const std::vector<DayHandle> days = lab.days();
    const std::span<const DayHandle> monday = std::span{days}.last(1);
    EXPECT_EQ(hex(run(acme("market_maker", kMonday), monday).digests),
              "c7eb451b0b743f7d 6ae9bf90f1f85c85 1c4e84dc022da0ee");
    EXPECT_EQ(hex(run(acme("imbalance", kMonday), monday).digests),
              "c7eb451b0b743f7d 84d6e3d50e269aaf 0bbc3c95e9d96772");
    EXPECT_EQ(hex(run(acme("market_maker", kFridayAndMonday), days).digests),
              "c7eb451b0b743f7d d623951ea5a8287b 7c3a3dc06d7a69d0");
    EXPECT_EQ(hex(run(acme("imbalance", kFridayAndMonday), days).digests),
              "c7eb451b0b743f7d 0a99d7d61f572c28 6fb2f87e540e3c7c");
}

// by_day and by_hour each add up to the PnL; the curve has one sample per exchange second with a market event, the
// last taken at the end; Max drawdown is the Portfolio's, as stepping the chain straight through gives it.
TEST(BacktestTest, TheDaysHoursAndCurveAddUpToThePnlWithOneSamplePerEventSecond)
{
    Lab lab{"backtest-curve"};
    lab.write("12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
    lab.write("12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
    const std::vector<DayHandle> days = lab.days();
    const RunSpec spec = acme("market_maker", kFridayAndMonday);
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
    RunSpec spec = acme("imbalance", kMonday, TradingWindow::FullDay);
    spec.strategy = strategy_spec("imbalance",
                                  std::vector<Param>{{"clip", 1}, {"max_position", 1'000'000'000}, {"cooldown_ns", 0}});
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
    const RunSpec spec = acme("market_maker", kFridayAndMonday);

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
    EXPECT_EQ(failed.by_day[0],
              run(acme("market_maker", {.day = {2019y / 12 / 27}, .count = 1}), std::span{days}.first(1)).by_day[0]);
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
    EXPECT_EQ(run(acme("market_maker", kMonday), std::span{invalid}.first(1)).reason, "out_of_order_timestamp");
    SessionConfig checked = lab_config();
    checked.check_invariants_every = 1;
    const RunResult crossing =
        run(acme("market_maker", {.day = {2019y / 12 / 31}, .count = 1}), std::span{invalid}.last(1), false, checked);
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
    const RunSpec spec = acme("imbalance", kFridayAndMonday);
    RunResult first = run(spec, days);
    RunResult second = run(spec, days, true);
    EXPECT_EQ(second.hashes, (std::vector<std::string>{days[0]->hash, days[1]->hash}));
    second.hashes.clear();
    first.wall_ms = second.wall_ms;
    first.cpu_ms = second.cpu_ms;
    EXPECT_EQ(first, second);
    EXPECT_GT(first.taker_fills, 0u);
}

// In backtest(): Monday's input ends at 09:30:02 with the market maker's bid half filled and its offer
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
        .executed(1, kNineThirty + 2 * kSecond, 3, 50, 1)  // rule (a): fills the bid 50
        .system_event(0, 16 * kHour, 'M');
    Lab lab{"backtest-close"};
    lab.write("12302019.NASDAQ_ITCH50.bin", itch.bytes());
    const RunResult r = run(acme("market_maker", kMonday), lab.days());

    ASSERT_EQ(r.fills.size(), 1u);
    EXPECT_EQ(r.fills[0].rule, FillCause::RuleAExecute);
    EXPECT_EQ(r.fills[0].quantity, Quantity{50});
    EXPECT_DOUBLE_EQ(r.fill_rate, 0.25);
    EXPECT_FALSE(r.partial);
}

}  // namespace
}  // namespace simulator::server::test
