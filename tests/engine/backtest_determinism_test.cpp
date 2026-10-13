#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <type_traits>
#include <vector>

#include "simulator/cli/replay_args.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/io/mapped_file.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "simulator/strategies/imbalance_taker.hpp"
#include "simulator/strategies/market_maker.hpp"

namespace simulator {
namespace {

constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

// A strategy, keeping every report it is delivered.
template <class S>
struct Recorded
{
    S strategy;
    std::vector<ExecutionReport> reports;

    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept
    {
        strategy.on_market(ctx, event);
    }
    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept
    {
        reports.push_back(report);
        strategy.on_report(ctx, report);
    }
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept
    {
        strategy.on_timer(ctx, timer_id);
    }
};

template <class Stats>
struct Outcome
{
    LoopReport loop;
    std::vector<ExecutionReport> reports;
    Stats stats;
    ExecutionStatistics execution;
    std::uint64_t digest{0};
    std::int64_t position{0};
    std::int64_t cash{0};
    std::int64_t fees{0};
    std::int64_t realised{0};
    std::int64_t pnl{0};
    std::int64_t max_drawdown{0};

    bool operator==(const Outcome&) const = default;
};

// A fresh book (symbol 0), portfolio, strategy and loop over `source`.
template <class S>
auto backtest(MarketEventSource& source, std::size_t capacity, S strategy, RiskLimits limits, FeeSchedule fees,
              LatencyConfig latency)
{
    MarketBook book{0, capacity};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, limits, fees};
    Recorded<S> recorded{strategy, {}};
    EventLoop<Recorded<S>> loop{source, book, execution, portfolio, recorded, latency};

    Outcome<std::remove_cvref_t<decltype(strategy.stats())>> run;
    run.loop = loop.run();
    run.reports = recorded.reports;
    run.stats = recorded.strategy.stats();
    run.execution = execution.statistics();
    run.digest = book.digest();
    run.position = portfolio.position();
    run.cash = portfolio.cash();
    run.fees = portfolio.fees();
    run.realised = portfolio.realised_pnl();
    run.pnl = portfolio.pnl();
    run.max_drawdown = portfolio.max_drawdown();
    return run;
}

Outcome<ImbalanceTakerStats> imbalance(MarketEventSource& source, std::size_t capacity,
                                       const ImbalanceTakerParams& params, LatencyConfig latency)
{
    return backtest(source, capacity, ImbalanceTaker{params},
                    RiskLimits{.max_position = params.max_position,
                               .max_order_quantity = Quantity{params.clip},
                               .max_open_orders = 1},
                    FeeSchedule{.taker_fee_per_share = 30}, latency);
}

Outcome<MarketMakerStats> market_maker(MarketEventSource& source, std::size_t capacity, const MarketMakerParams& params,
                                       LatencyConfig latency)
{
    return backtest(source, capacity, MarketMaker{params},
                    RiskLimits{.max_position = params.max_position,
                               .max_order_quantity = Quantity{params.quote_size},
                               .max_open_orders = 2},
                    FeeSchedule{.maker_fee_per_share = -2, .taker_fee_per_share = 30}, latency);
}

// The history digest after replaying `source` alone.
std::uint64_t replay_digest(MarketEventSource& source, std::size_t capacity)
{
    MarketBook book{0, capacity};
    MarketReplay::run(source, book, {});
    return book.digest();
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
            events.push_back(MarketEvent{.timestamp = ts,
                                         .order_id = next_id,
                                         .price = Price{side == Side::Buy ? 1'000'000 - offset : 1'000'000 + offset},
                                         .quantity = Quantity{quantity},
                                         .type = MarketEventType::Add,
                                         .side = side});
            live.push_back({next_id++, quantity});
            continue;
        }

        const std::size_t pick = rng() % live.size();
        Live& order = live[pick];
        const bool execute = roll < 7;
        const std::uint64_t quantity = execute ? 1 + rng() % order.remaining : order.remaining;
        events.push_back(MarketEvent{.timestamp = ts,
                                     .order_id = order.id,
                                     .quantity = Quantity{quantity},
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
    const Outcome first = imbalance(first_source, 4'096, params, latency);
    const Outcome second = imbalance(second_source, 4'096, params, latency);

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
    EXPECT_EQ(first.digest, replay_digest(replay_source, 4'096));
}

TEST(BacktestDeterminismTest, SyntheticMarketMakerRunsAreIdentical)
{
    const std::vector<MarketEvent> events = synthetic_market(13, 20'000);
    const MarketMakerParams params{.quote_size = 100, .max_position = 300, .requote_cooldown_ns = 400};
    const LatencyConfig latency{.order_latency = 300, .report_latency = 300};

    VectorSource first_source{events};
    VectorSource second_source{events};
    const Outcome first = market_maker(first_source, 4'096, params, latency);
    const Outcome second = market_maker(second_source, 4'096, params, latency);

    ASSERT_TRUE(first.loop.replay.succeeded());
    EXPECT_EQ(first.loop.replay.rejected(), 0u);
    EXPECT_GT(first.stats.bought, 0u);
    EXPECT_GT(first.stats.sold, 0u);
    EXPECT_GT(first.stats.cancels, 0u);
    EXPECT_GT(first.execution.maker_quantity, 0u);
    EXPECT_EQ(first.execution.taker_quantity, 0u);
    EXPECT_LT(first.fees, 0);  // rebates only
    EXPECT_LE(std::abs(first.position), params.max_position);

    EXPECT_EQ(first.reports, second.reports);
    EXPECT_EQ(first, second);

    // Resting orders, queue tracking and maker fills never touch the history.
    VectorSource replay_source{events};
    EXPECT_EQ(first.digest, replay_digest(replay_source, 4'096));
}

// Real NASDAQ data, never committed: see ItchRealDataTest.
std::string_view env_or(const char* name, std::string_view fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string_view{value} : fallback;
}

struct RealData
{
    MappedFile file;
    ItchSourceConfig config;
};

// False (and the test skips) without CHRONOS_ITCH_FILE.
bool open_real_data(RealData& data)
{
    const char* path = std::getenv("CHRONOS_ITCH_FILE");
    if (path == nullptr || *path == '\0') {
        return false;
    }
    std::chrono::year_month_day date{};
    EXPECT_TRUE(parse_date(env_or("CHRONOS_ITCH_DATE", "2019-12-30"), date));
    EXPECT_TRUE(data.file.open(path)) << data.file.error();
    data.config = ItchSourceConfig{.ticker = env_or("CHRONOS_ITCH_SYMBOL", "AAPL"),
                                   .session_date = date,
                                   .utc_offset = us_eastern_utc_offset(date),
                                   .allow_truncated_tail = true};
    return true;
}

// Two runs of `run_once` agree, have no replay rejections, and leave the history's digest.
template <class Run>
void expect_deterministic(const RealData& data, Run run_once, const char* name)
{
    const auto first = run_once();
    const auto second = run_once();

    std::printf("[ RealData ] %.*s %s: %llu events, %llu reports, pnl %lld, digest %016llx\n",
                static_cast<int>(data.config.ticker.size()), data.config.ticker.data(), name,
                static_cast<unsigned long long>(first.loop.replay.events),
                static_cast<unsigned long long>(first.reports.size()), static_cast<long long>(first.pnl),
                static_cast<unsigned long long>(first.digest));

    ASSERT_TRUE(first.loop.replay.succeeded()) << first.loop.replay.message;
    EXPECT_EQ(first.loop.replay.rejected(), 0u);
    EXPECT_FALSE(first.reports.empty());
    EXPECT_EQ(first.reports, second.reports);
    EXPECT_EQ(first, second);

    InstrumentRegistry registry{1};
    ItchSource source{data.file.bytes(), data.config, registry};
    EXPECT_EQ(first.digest, replay_digest(source, 1'048'576));
}

// The CLI's defaults throughout.
constexpr LatencyConfig kCliLatency{.order_latency = 50'000, .report_latency = 50'000};

TEST(BacktestDeterminismTest, RealDataRunsAreIdenticalWithoutRejections)
{
    RealData data;
    if (!open_real_data(data)) {
        GTEST_SKIP() << "set CHRONOS_ITCH_FILE to a NASDAQ ITCH 5.0 file";
    }
    const ImbalanceTakerParams params{.threshold = 3.0, .clip = 100, .max_position = 500, .cooldown_ns = 100'000'000};
    expect_deterministic(
        data,
        [&] {
            InstrumentRegistry registry{1};
            ItchSource source{data.file.bytes(), data.config, registry};
            return imbalance(source, 1'048'576, params, kCliLatency);  // ItchSource registers id 0
        },
        "imbalance");
}

TEST(BacktestDeterminismTest, RealDataMarketMakerRunsAreIdenticalWithoutRejections)
{
    RealData data;
    if (!open_real_data(data)) {
        GTEST_SKIP() << "set CHRONOS_ITCH_FILE to a NASDAQ ITCH 5.0 file";
    }
    const MarketMakerParams params{.quote_size = 100, .max_position = 500, .requote_cooldown_ns = 100'000'000};
    expect_deterministic(
        data,
        [&] {
            InstrumentRegistry registry{1};
            ItchSource source{data.file.bytes(), data.config, registry};
            return market_maker(source, 1'048'576, params, kCliLatency);
        },
        "market maker");
}

} // namespace
} // namespace simulator
