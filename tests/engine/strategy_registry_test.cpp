#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "simulator/engine/event_loop.hpp"
#include "simulator/engine/strategy_registry.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "support/synthetic_market.hpp"

namespace simulator {
namespace {

constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

// Every registered strategy, run the way the CLI and the Lab run it: through the registry, behind AnyStrategy.
struct Outcome
{
    LoopReport loop{};
    std::uint64_t book_digest{0};
    std::uint64_t fills_digest{0};
    std::int64_t pnl{0};
    std::int64_t position{0};
    ExecutionStatistics execution{};
    std::vector<Stat> stats{};

    bool operator==(const Outcome&) const = default;
};

Outcome run(const StrategyInfo& info, const std::vector<MarketEvent>& events)
{
    ParamList params;
    std::string bad;
    EXPECT_TRUE(resolve_params(info.params, {}, params, bad)) << bad;
    const DaySession session{.open = 0, .close = events.back().timestamp + 1};
    const std::unique_ptr<AnyStrategy> strategy = info.make(params, {.sessions = {&session, 1}, .price_scale = 4});
    VectorSource source{events};
    MarketBook book{0, 4'096};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, info.limits(params),
                        FeeSchedule{.maker_fee_per_share = -2, .taker_fee_per_share = 30}};
    EventLoop<AnyStrategy> loop{source,    book,      execution,
                                portfolio, *strategy, {.order_latency = 300, .report_latency = 300}};
    Outcome out{.loop = loop.run(),
                .book_digest = book.digest(),
                .fills_digest = portfolio.fills_digest(),
                .pnl = portfolio.pnl(),
                .position = portfolio.position(),
                .execution = execution.statistics()};
    strategy->stats(out.stats);
    return out;
}

// A new plugin is covered here with no test of its own: its declaration is sound, its defaults trade under its own
// limits, two runs are identical, and the history is the plain replay's.
TEST(StrategyRegistryTest, EveryPluginIsSoundDeterministicAndLeavesTheHistoryAlone)
{
    const std::vector<StrategyInfo>& strategies = registered_strategies();
    ASSERT_GE(strategies.size(), 3u);  // strategies/: imbalance, market_maker, orb
    const std::vector<MarketEvent> events = synthetic_market(7, 20'000);
    VectorSource plain{events};
    MarketBook history{0, 4'096};
    MarketReplay::run(plain, history, {});

    for (std::size_t i = 0; i < strategies.size(); ++i) {
        const StrategyInfo& info = strategies[i];
        SCOPED_TRACE(info.name);
        EXPECT_TRUE(valid_strategy_name(info.name));
        EXPECT_FALSE(info.label.empty());
        if (i > 0) {
            EXPECT_LT(strategies[i - 1].name, info.name);  // sorted, so unique
        }
        for (const ParamInfo& p : info.params) {
            EXPECT_TRUE(valid_strategy_name(p.name)) << p.name;
            EXPECT_TRUE(param_fits(p, p.default_value)) << p.name;
            EXPECT_TRUE(param_fits(p, p.min)) << p.name;
        }
        ParamList params;
        std::string bad;
        ASSERT_TRUE(resolve_params(info.params, {}, params, bad)) << bad;
        const RiskLimits limits = info.limits(params);
        EXPECT_GE(limits.max_position, 0);
        EXPECT_GT(limits.max_order_quantity.value(), 0u);
        EXPECT_GT(limits.max_open_orders, 0u);
        if (info.check) {
            EXPECT_EQ(info.check(params, limits), "");  // its defaults can trade
        }

        const Outcome first = run(info, events);
        ASSERT_TRUE(first.loop.replay.succeeded());
        EXPECT_EQ(first, run(info, events));
        EXPECT_EQ(first.book_digest, history.digest());
    }
}

TEST(StrategyRegistryTest, TheToyStrategiesTradeOnTheSyntheticMarket)
{
    const std::vector<MarketEvent> events = synthetic_market(7, 20'000);
    EXPECT_GT(run(*find_strategy("imbalance"), events).execution.taker_fills, 0u);
    EXPECT_GT(run(*find_strategy("market_maker"), events).execution.maker_fills, 0u);
    EXPECT_EQ(find_strategy("nope"), nullptr);
}

TEST(StrategyRegistryTest, ResolvesParamsByNameTypeAndRange)
{
    const std::vector<ParamInfo> params{
        {.name = "size", .default_value = std::int64_t{10}, .min = std::int64_t{1}, .max = std::int64_t{100}},
        {.name = "ratio", .default_value = 2.5, .min = 1.0, .max = std::numeric_limits<double>::infinity()}};
    ParamList out;
    std::string bad;
    ASSERT_TRUE(resolve_params(params, {}, out, bad));
    EXPECT_EQ(out, (ParamList{{"ratio", 2.5}, {"size", std::int64_t{10}}}));  // sorted by name

    ASSERT_TRUE(
        resolve_params(params, std::vector<Param>{{"ratio", std::int64_t{3}}, {"size", std::int64_t{100}}}, out, bad));
    EXPECT_EQ(out, (ParamList{{"ratio", 3.0}, {"size", std::int64_t{100}}}));  // an integer for a real param

    const auto refused = [&](const Param& given) {
        bad.clear();
        return !resolve_params(params, std::vector<Param>{given}, out, bad) && bad == given.name;
    };
    EXPECT_TRUE(refused({"size", std::int64_t{0}}));
    EXPECT_TRUE(refused({"size", std::int64_t{101}}));
    EXPECT_TRUE(refused({"size", 5.0}));  // a real for an integer param
    EXPECT_TRUE(refused({"ratio", 0.5}));
    EXPECT_TRUE(refused({"ratio", std::numeric_limits<double>::infinity()}));
    EXPECT_TRUE(refused({"ratio", std::numeric_limits<double>::quiet_NaN()}));
    EXPECT_TRUE(refused({"other", std::int64_t{1}}));

    ParamValue value;
    EXPECT_TRUE(parse_param(params[0], "42", value));
    EXPECT_EQ(value, ParamValue{std::int64_t{42}});
    EXPECT_TRUE(parse_param(params[1], "1e3", value));
    EXPECT_EQ(value, ParamValue{1'000.0});
    EXPECT_FALSE(parse_param(params[0], "4.2", value));
    EXPECT_FALSE(parse_param(params[0], "", value));
    EXPECT_FALSE(parse_param(params[1], " 3", value));
    EXPECT_FALSE(parse_param(params[1], "inf", value));
    EXPECT_FALSE(parse_param(params[1], "3x", value));
}

TEST(StrategyRegistryTest, ValidatesNames)
{
    EXPECT_TRUE(valid_strategy_name("my_strategy_2"));
    EXPECT_FALSE(valid_strategy_name(""));
    EXPECT_FALSE(valid_strategy_name("MyStrategy"));
    EXPECT_FALSE(valid_strategy_name("my-strategy"));
    EXPECT_FALSE(valid_strategy_name("script:x"));
    EXPECT_FALSE(valid_strategy_name(std::string(41, 'a')));
}

// A plugin that reuses a name, or has no limits, stops the program at start-up, before any run can mix them up.
TEST(StrategyRegistryDeathTest, ABadRegistrationAborts)
{
    StrategyInfo copy = *find_strategy("orb");
    EXPECT_DEATH(register_strategy(copy), "registered twice");
    copy.name = "orb_without_limits";
    copy.limits = nullptr;
    EXPECT_DEATH(register_strategy(copy), "needs limits");
}

}  // namespace
}  // namespace simulator
