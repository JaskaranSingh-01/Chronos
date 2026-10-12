#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <vector>

#include "simulator/cli/replay_args.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "simulator/strategies/imbalance_taker.hpp"

namespace simulator {
namespace {

constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

// ImbalanceTaker, keeping every report it is delivered.
struct Recorded
{
    ImbalanceTaker taker;
    std::vector<ExecutionReport> reports;

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept { taker.on_market(ctx, event); }
    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        reports.push_back(report);
        taker.on_report(ctx, report);
    }
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept { taker.on_timer(ctx, timer_id); }
};

struct Outcome
{
    LoopReport loop;
    std::vector<ExecutionReport> reports;
    ImbalanceTakerStats stats;
    std::uint64_t digest{0};
    std::int64_t position{0};
    std::int64_t cash{0};
    std::int64_t fees{0};
    std::int64_t realised{0};
    std::int64_t pnl{0};
    std::int64_t max_drawdown{0};

    bool operator==(const Outcome&) const = default;
};

// A fresh book, portfolio, strategy and loop over `source`.
Outcome backtest(MarketEventSource& source, SymbolId symbol, std::size_t capacity, const ImbalanceTakerParams& params,
             LatencyConfig latency)
{
    MarketBook book{symbol, capacity};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity,
                        RiskLimits{.max_position = params.max_position, .max_order_quantity = Quantity{params.clip},
                                   .max_open_orders = 1},
                        FeeSchedule{.taker_fee_per_share = 30}};
    Recorded strategy{ImbalanceTaker{params}, {}};
    EventLoop<Recorded> loop{source, book, execution, portfolio, strategy, latency};

    Outcome run;
    run.loop = loop.run();
    run.reports = strategy.reports;
    run.stats = strategy.taker.stats();
    run.digest = book.digest();
    run.position = portfolio.position();
    run.cash = portfolio.cash();
    run.fees = portfolio.fees();
    run.realised = portfolio.realised_pnl();
    run.pnl = portfolio.pnl();
    run.max_drawdown = portfolio.max_drawdown();
    return run;
}

// A quoting market around a fixed mid: three levels a side, 6-40 live orders
// of 10-400 shares, partial executions and deletes, timestamps 0-199 ns apart
// (ties included).
std::vector<MarketEvent> synthetic_market(std::uint64_t seed, std::size_t count)
{
    struct Live
    {
        OrderId id;
        std::uint64_t remaining;
    };

    std::mt19937_64 rng{seed};
    std::vector<MarketEvent> events;
    std::vector<Live> live;
    Timestamp ts = 1'000;
    OrderId next_id = 1;

    while (events.size() < count) {
        ts += static_cast<Timestamp>(rng() % 200);
        const std::uint64_t roll = rng() % 10;

        if (live.size() < 6 || (roll < 4 && live.size() < 40)) {
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            const auto offset = static_cast<std::int64_t>(100 * (1 + rng() % 3));
            const std::uint64_t quantity = 10 * (1 + rng() % 40);
            events.push_back(MarketEvent{.timestamp = ts, .order_id = next_id,
                                         .price = Price{side == Side::Buy ? 1'000'000 - offset : 1'000'000 + offset},
                                         .quantity = Quantity{quantity}, .type = MarketEventType::Add, .side = side});
            live.push_back({next_id++, quantity});
            continue;
        }

        const std::size_t pick = rng() % live.size();
        Live& order = live[pick];
        const bool execute = roll < 7;
        const std::uint64_t quantity = execute ? 1 + rng() % order.remaining : order.remaining;
        events.push_back(MarketEvent{.timestamp = ts, .order_id = order.id, .quantity = Quantity{quantity},
                                     .type = execute ? MarketEventType::Execute : MarketEventType::Delete});
        order.remaining -= quantity;
        if (order.remaining == 0) {
            live[pick] = live.back();
            live.pop_back();
        }
    }
    return events;
}

TEST(BacktestDeterminismTest, SyntheticRunsAreIdentical)
{
    const std::vector<MarketEvent> events = synthetic_market(11, 20'000);
    // Cooldown shorter than the 600 ns round trip, so max_open_orders = 1 rejects some sends.
    const ImbalanceTakerParams params{.threshold = 3.0, .clip = 100, .max_position = 300, .cooldown_ns = 400};
    const LatencyConfig latency{.order_latency = 300, .report_latency = 300};

    VectorSource first_source{events};
    VectorSource second_source{events};
    const Outcome first = backtest(first_source, 0, 4'096, params, latency);
    const Outcome second = backtest(second_source, 0, 4'096, params, latency);

    ASSERT_TRUE(first.loop.replay.succeeded());
    EXPECT_EQ(first.loop.replay.rejected(), 0u);
    EXPECT_GT(first.stats.bought, 0u);
    EXPECT_GT(first.stats.sold, 0u);
    EXPECT_GT(first.stats.expired, 0u);
    EXPECT_GT(first.stats.risk_rejects, 0u);
    EXPECT_NE(first.fees, 0);

    EXPECT_EQ(first.reports, second.reports);
    EXPECT_EQ(first, second);

    // Our orders never touch the historical book.
    VectorSource replay_source{events};
    MarketBook replay_book{0, 4'096};
    MarketReplay::run(replay_source, replay_book, {});
    EXPECT_EQ(first.digest, replay_book.digest());
}

// Real NASDAQ data, never committed: see ItchRealDataTest.
std::string_view env_or(const char* name, std::string_view fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string_view{value} : fallback;
}

TEST(BacktestDeterminismTest, RealDataRunsAreIdenticalWithoutRejections)
{
    const char* path = std::getenv("CHRONOS_ITCH_FILE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP() << "set CHRONOS_ITCH_FILE to a NASDAQ ITCH 5.0 file";
    }

    std::chrono::year_month_day date{};
    ASSERT_TRUE(parse_date(env_or("CHRONOS_ITCH_DATE", "2019-12-30"), date));
    MappedFile file;
    ASSERT_TRUE(file.open(path)) << file.error();
    const ItchSourceConfig config{.ticker = env_or("CHRONOS_ITCH_SYMBOL", "AAPL"), .session_date = date,
                                  .utc_offset = us_eastern_utc_offset(date), .allow_truncated_tail = true};

    // The CLI's defaults.
    const ImbalanceTakerParams params{.threshold = 3.0, .clip = 100, .max_position = 500,
                                      .cooldown_ns = 100'000'000};
    const LatencyConfig latency{.order_latency = 50'000, .report_latency = 50'000};

    auto run_once = [&] {
        InstrumentRegistry registry{1};
        ItchSource source{file.bytes(), config, registry};
        return backtest(source, 0, 1'048'576, params, latency);  // ItchSource registers id 0
    };
    const Outcome first = run_once();
    const Outcome second = run_once();

    std::printf("[ RealData ] %.*s backtest: %llu events, %llu sent, %llu reports, pnl %lld, digest %016llx\n",
                static_cast<int>(config.ticker.size()), config.ticker.data(),
                static_cast<unsigned long long>(first.loop.replay.events),
                static_cast<unsigned long long>(first.stats.sent),
                static_cast<unsigned long long>(first.reports.size()), static_cast<long long>(first.pnl),
                static_cast<unsigned long long>(first.digest));

    ASSERT_TRUE(first.loop.replay.succeeded()) << first.loop.replay.message;
    EXPECT_EQ(first.loop.replay.rejected(), 0u);
    EXPECT_GT(first.stats.sent, 0u);
    EXPECT_EQ(first.reports, second.reports);
    EXPECT_EQ(first, second);

    InstrumentRegistry registry{1};
    ItchSource source{file.bytes(), config, registry};
    MarketBook book{0, 1'048'576};
    MarketReplay::run(source, book, {});
    EXPECT_EQ(first.digest, book.digest());
}

} // namespace
} // namespace simulator
