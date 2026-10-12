#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "simulator/cli/replay_args.hpp"

namespace simulator {
namespace {

using namespace std::chrono;

struct Parsed
{
    bool ok;
    ReplayArgs args;
    std::string error;
};

Parsed parse(std::vector<std::string_view> argv)
{
    Parsed parsed{};
    parsed.ok = parse_replay_args(argv, parsed.args, parsed.error);
    return parsed;
}

TEST(ReplayArgsTest, ParsesEveryOption)
{
    const Parsed p = parse({"replay", "day.bin", "--symbol", "MSFT", "--date", "2019-12-30",
                            "--utc-offset", "+05:30", "--allow-truncated-tail", "--depth", "10",
                            "--check-invariants", "10000", "--capacity", "4096"});

    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(p.args.file, "day.bin");
    EXPECT_EQ(p.args.symbol, "MSFT");
    EXPECT_EQ(p.args.date, 2019y / December / 30);
    EXPECT_EQ(p.args.utc_offset, 5h + 30min);
    EXPECT_TRUE(p.args.allow_truncated_tail);
    EXPECT_EQ(p.args.depth, 10u);
    EXPECT_EQ(p.args.check_invariants_every, 10'000u);
    EXPECT_EQ(p.args.capacity, 4'096u);
}

TEST(ReplayArgsTest, DefaultsAndUsEasternOffsetFromDate)
{
    const Parsed winter = parse({"replay", "--symbol", "AAPL", "--date", "2019-12-30", "day.bin"});

    ASSERT_TRUE(winter.ok) << winter.error;
    EXPECT_EQ(winter.args.file, "day.bin");  // the file may come after the flags
    EXPECT_EQ(winter.args.utc_offset, -5h);
    EXPECT_FALSE(winter.args.allow_truncated_tail);
    EXPECT_EQ(winter.args.depth, 5u);
    EXPECT_EQ(winter.args.check_invariants_every, 0u);
    EXPECT_EQ(winter.args.capacity, 1'048'576u);

    const Parsed summer = parse({"replay", "day.bin", "--symbol", "AAPL", "--date", "2012-06-21"});
    ASSERT_TRUE(summer.ok) << summer.error;
    EXPECT_EQ(summer.args.utc_offset, -4h);

    const Parsed negative = parse({"replay", "day.bin", "--symbol", "AAPL", "--date", "2012-06-21",
                                   "--utc-offset", "-03:30"});
    ASSERT_TRUE(negative.ok) << negative.error;
    EXPECT_EQ(negative.args.utc_offset, -(3h + 30min));
}

TEST(ReplayArgsTest, RejectsMissingRequiredArguments)
{
    EXPECT_FALSE(parse({}).ok);
    EXPECT_FALSE(parse({"play", "day.bin", "--symbol", "AAPL", "--date", "2019-12-30"}).ok);
    EXPECT_FALSE(parse({"replay", "--symbol", "AAPL", "--date", "2019-12-30"}).ok);  // no file
    EXPECT_FALSE(parse({"replay", "day.bin", "--date", "2019-12-30"}).ok);           // no symbol
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol", "AAPL"}).ok);               // no date
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol"}).ok);                       // no value

    const Parsed p = parse({"replay", "day.bin", "--date", "2019-12-30"});
    EXPECT_NE(p.error.find("--symbol"), std::string::npos) << p.error;
}

TEST(ReplayArgsTest, RejectsBadValues)
{
    auto with = [](std::string_view flag, std::string_view value) {
        return parse({"replay", "day.bin", "--symbol", "AAPL", "--date", "2019-12-30", flag, value}).ok;
    };

    EXPECT_FALSE(with("--depth", "five"));
    EXPECT_FALSE(with("--depth", "5x"));
    EXPECT_FALSE(with("--depth", "-1"));
    EXPECT_FALSE(with("--capacity", "0"));
    EXPECT_FALSE(with("--check-invariants", ""));
    EXPECT_FALSE(with("--check-invariants", "99999999999999999999999"));
    EXPECT_FALSE(with("--utc-offset", "05:00"));    // no sign
    EXPECT_FALSE(with("--utc-offset", "-5:00"));
    EXPECT_FALSE(with("--utc-offset", "-05:60"));
    EXPECT_FALSE(with("--utc-offset", "+24:00"));
    EXPECT_FALSE(with("--date", "2019-02-30"));
    EXPECT_FALSE(with("--date", "30-12-2019"));
    EXPECT_FALSE(with("--symbol", "TOOLONGSYM"));  // ITCH tickers are 1-8 chars
    EXPECT_FALSE(with("--frobnicate", "1"));
    EXPECT_FALSE(parse({"replay", "a.bin", "b.bin", "--symbol", "AAPL", "--date", "2019-12-30"}).ok);
}

TEST(ReplayArgsTest, ParsesBacktestOptions)
{
    const Parsed p = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30",
                            "--allow-truncated-tail", "--capacity", "4096", "--order-latency-us", "20",
                            "--report-latency-us", "30", "--threshold", "2.5", "--clip", "200",
                            "--max-position", "1000", "--cooldown-ms", "5", "--taker-fee-ticks", "30"});

    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(p.args.command, CliCommand::Backtest);
    EXPECT_EQ(p.args.symbol, "SPY");
    EXPECT_TRUE(p.args.allow_truncated_tail);
    EXPECT_EQ(p.args.capacity, 4'096u);
    EXPECT_EQ(p.args.backtest.order_latency_us, 20u);
    EXPECT_EQ(p.args.backtest.report_latency_us, 30u);
    EXPECT_EQ(p.args.backtest.threshold, 2.5);
    EXPECT_EQ(p.args.backtest.clip, 200u);
    EXPECT_EQ(p.args.backtest.max_position, 1'000);
    EXPECT_EQ(p.args.backtest.cooldown_ms, 5u);
    EXPECT_EQ(p.args.backtest.taker_fee_ticks, 30u);
}

TEST(ReplayArgsTest, BacktestDefaultsAndFlagsOnlyForBacktest)
{
    const Parsed p = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(p.args.backtest.order_latency_us, 50u);
    EXPECT_EQ(p.args.backtest.report_latency_us, 50u);
    EXPECT_EQ(p.args.backtest.threshold, 3.0);
    EXPECT_EQ(p.args.backtest.clip, 100u);
    EXPECT_EQ(p.args.backtest.max_position, 500);
    EXPECT_EQ(p.args.backtest.cooldown_ms, 100u);
    EXPECT_EQ(p.args.backtest.taker_fee_ticks, 0u);

    const Parsed replay = parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(replay.ok) << replay.error;
    EXPECT_EQ(replay.args.command, CliCommand::Replay);
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--clip", "100"}).ok);

    auto with = [](std::string_view flag, std::string_view value) {
        return parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", flag, value}).ok;
    };
    EXPECT_FALSE(with("--clip", "0"));
    EXPECT_FALSE(with("--threshold", "0.5"));  // below 1 both sides would trigger
    EXPECT_FALSE(with("--threshold", "three"));
    EXPECT_FALSE(with("--threshold", "nan"));
    EXPECT_FALSE(with("--threshold", "3x"));
    EXPECT_FALSE(with("--max-position", "-1"));
    EXPECT_FALSE(with("--order-latency-us", "1000000001"));  // ns would overflow past this
    EXPECT_FALSE(with("--cooldown-ms", "1000000001"));
    EXPECT_FALSE(with("--taker-fee-ticks", "-30"));
    EXPECT_TRUE(with("--threshold", "1"));
    EXPECT_TRUE(with("--max-position", "0"));
}

TEST(ReplayArgsTest, ParsesMarketMakerOptions)
{
    const Parsed p = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "mm",
                            "--quote-size", "200", "--max-position", "800", "--requote-ms", "250",
                            "--maker-fee-ticks", "-20", "--taker-fee-ticks", "30", "--max-open-orders", "4"});

    ASSERT_TRUE(p.ok) << p.error;
    const BacktestArgs& b = p.args.backtest;
    EXPECT_EQ(b.strategy, StrategyKind::MarketMaker);
    EXPECT_EQ(b.quote_size, 200u);
    EXPECT_EQ(b.max_position, 800);
    EXPECT_EQ(b.requote_ms, 250u);
    EXPECT_EQ(b.maker_fee_ticks, -20);  // a rebate
    EXPECT_EQ(b.taker_fee_ticks, 30u);
    EXPECT_EQ(b.max_open_orders, 4u);
}

TEST(ReplayArgsTest, StrategyDefaultsAndBadValues)
{
    const Parsed p = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(p.ok) << p.error;
    const BacktestArgs& b = p.args.backtest;
    EXPECT_EQ(b.strategy, StrategyKind::Imbalance);
    EXPECT_EQ(b.quote_size, 100u);
    EXPECT_EQ(b.requote_ms, 100u);
    EXPECT_EQ(b.maker_fee_ticks, 0);
    EXPECT_EQ(b.max_open_orders, 0u);  // the strategy's own default
    EXPECT_EQ(default_max_open_orders(StrategyKind::Imbalance), 1u);
    EXPECT_EQ(default_max_open_orders(StrategyKind::MarketMaker), 2u);

    auto with = [](std::string_view flag, std::string_view value) {
        return parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", flag, value}).ok;
    };
    EXPECT_TRUE(with("--strategy", "imbalance"));
    EXPECT_FALSE(with("--strategy", "MM"));
    EXPECT_FALSE(with("--quote-size", "0"));
    EXPECT_FALSE(with("--requote-ms", "1000000001"));
    EXPECT_FALSE(with("--maker-fee-ticks", "-1000001"));
    EXPECT_FALSE(with("--maker-fee-ticks", "+5"));
    EXPECT_FALSE(with("--max-open-orders", "0"));
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "mm"}).ok);
}

TEST(ReplayArgsTest, ParsesDatesAndOffsets)
{
    year_month_day date{};
    EXPECT_TRUE(parse_date("2024-02-29", date));
    EXPECT_EQ(date, 2024y / February / 29);
    EXPECT_FALSE(parse_date("2023-02-29", date));
    EXPECT_FALSE(parse_date("2019-1-30", date));
    EXPECT_FALSE(parse_date("2019-12-30 ", date));

    minutes offset{};
    EXPECT_TRUE(parse_utc_offset("-05:00", offset));
    EXPECT_EQ(offset, -5h);
    EXPECT_TRUE(parse_utc_offset("+00:00", offset));
    EXPECT_EQ(offset, 0min);
    EXPECT_FALSE(parse_utc_offset("-05", offset));
    EXPECT_FALSE(parse_utc_offset("-05:0a", offset));
}

} // namespace
} // namespace simulator
