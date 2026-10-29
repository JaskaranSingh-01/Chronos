#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
    const Parsed p = parse({"replay", "day.bin", "--symbol", "MSFT", "--date", "2019-12-30", "--utc-offset", "+05:30",
                            "--allow-truncated-tail", "--depth", "10", "--check-invariants", "10000", "--capacity",
                            "4096", "--book", "list", "--threads", "1"});

    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(p.args.file, "day.bin");
    EXPECT_EQ(p.args.symbol, "MSFT");
    EXPECT_EQ(p.args.date, 2019y / December / 30);
    EXPECT_EQ(p.args.utc_offset, 5h + 30min);
    EXPECT_TRUE(p.args.allow_truncated_tail);
    EXPECT_EQ(p.args.depth, 10u);
    EXPECT_EQ(p.args.check_invariants_every, 10'000u);
    EXPECT_EQ(p.args.capacity, 4'096u);
    EXPECT_EQ(p.args.book, BookBackend::List);
    EXPECT_EQ(p.args.threads, 1u);
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
    EXPECT_EQ(winter.args.book, BookBackend::Ladder);
    EXPECT_EQ(winter.args.threads, 2u);

    const Parsed summer = parse({"replay", "day.bin", "--symbol", "AAPL", "--date", "2012-06-21"});
    ASSERT_TRUE(summer.ok) << summer.error;
    EXPECT_EQ(summer.args.utc_offset, -4h);

    const Parsed negative =
        parse({"replay", "day.bin", "--symbol", "AAPL", "--date", "2012-06-21", "--utc-offset", "-03:30"});
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
    EXPECT_FALSE(with("--depth", "1001"));  // print_book allocates depth levels per side
    EXPECT_TRUE(with("--depth", "1000"));
    EXPECT_FALSE(with("--capacity", "1073741825"));  // 2^30 + 1
    EXPECT_TRUE(with("--capacity", "1073741824"));
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
    EXPECT_FALSE(with("--book", "tree"));
    EXPECT_TRUE(with("--book", "list"));
    EXPECT_FALSE(with("--threads", "3"));
    EXPECT_FALSE(with("--threads", "0"));
    EXPECT_TRUE(with("--threads", "1"));
    EXPECT_FALSE(parse({"replay", "a.bin", "b.bin", "--symbol", "AAPL", "--date", "2019-12-30"}).ok);
}

// A param's value in `b`, or -1 for none.
double param(const BacktestArgs& b, std::string_view name)
{
    const ParamValue* value = find_param(b.params, name);
    if (value == nullptr) {
        return -1;
    }
    return std::holds_alternative<double>(*value) ? std::get<double>(*value)
                                                  : static_cast<double>(std::get<std::int64_t>(*value));
}

TEST(ReplayArgsTest, ParsesBacktestOptions)
{
    const Parsed p = parse({"backtest",
                            "day.bin",
                            "--symbol",
                            "SPY",
                            "--date",
                            "2019-12-30",
                            "--allow-truncated-tail",
                            "--capacity",
                            "4096",
                            "--order-latency-us",
                            "20",
                            "--report-latency-us",
                            "30",
                            "--param",
                            "threshold=2.5",
                            "--param",
                            "clip=200",
                            "--max-position",
                            "1000",
                            "--param",
                            "cooldown_ns=5000000",
                            "--taker-fee-ticks",
                            "30"});

    ASSERT_TRUE(p.ok) << p.error;
    const BacktestArgs& b = p.args.backtest;
    EXPECT_EQ(p.args.command, CliCommand::Backtest);
    EXPECT_EQ(p.args.symbol, "SPY");
    EXPECT_TRUE(p.args.allow_truncated_tail);
    EXPECT_EQ(p.args.capacity, 4'096u);
    EXPECT_EQ(b.order_latency_ns, 20'000u);
    EXPECT_EQ(b.report_latency_ns, 30'000u);
    EXPECT_EQ(b.strategy, "imbalance");
    EXPECT_EQ(param(b, "threshold"), 2.5);
    EXPECT_EQ(param(b, "clip"), 200);
    EXPECT_EQ(param(b, "max_position"), 500);  // the param; --max-position is the risk limit
    EXPECT_EQ(param(b, "cooldown_ns"), 5'000'000);
    EXPECT_EQ(b.limits.max_position, 1'000);
    EXPECT_EQ(b.limits.max_order_quantity.value(), 200u);  // the strategy's own: its clip
    EXPECT_EQ(b.limits.max_open_orders, 1u);
    EXPECT_EQ(b.taker_fee_ticks, 30u);
}

TEST(ReplayArgsTest, BacktestDefaultsAndFlagsOnlyForBacktest)
{
    const Parsed p = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(p.ok) << p.error;
    const BacktestArgs& b = p.args.backtest;
    EXPECT_EQ(b.order_latency_ns, 50'000u);
    EXPECT_EQ(b.report_latency_ns, 50'000u);
    EXPECT_EQ(b.strategy, "imbalance");
    EXPECT_EQ(param(b, "threshold"), 3.0);
    EXPECT_EQ(param(b, "clip"), 100);
    EXPECT_EQ(param(b, "max_position"), 500);
    EXPECT_EQ(param(b, "cooldown_ns"), 100'000'000);
    EXPECT_EQ(b.limits.max_position, 500);  // the strategy's own, from its params
    EXPECT_EQ(b.taker_fee_ticks, 0u);
    EXPECT_EQ(b.maker_fee_ticks, 0);

    const Parsed replay = parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(replay.ok) << replay.error;
    EXPECT_EQ(replay.args.command, CliCommand::Replay);
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--param", "clip=1"}).ok);
    EXPECT_FALSE(parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "orb"}).ok);

    auto with = [](std::string_view flag, std::string_view value) {
        return parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", flag, value}).ok;
    };
    EXPECT_FALSE(with("--param", "clip=0"));
    EXPECT_FALSE(with("--param", "threshold=0.5"));  // below 1 both sides would trigger
    EXPECT_FALSE(with("--param", "threshold=three"));
    EXPECT_FALSE(with("--param", "threshold=nan"));
    EXPECT_FALSE(with("--param", "threshold=inf"));
    EXPECT_FALSE(with("--param", "threshold=3x"));
    EXPECT_FALSE(with("--param", "threshold= 3"));
    EXPECT_FALSE(with("--param", "clip=2.5"));  // an integer param takes only an integer
    EXPECT_FALSE(with("--param", "clip"));
    EXPECT_FALSE(with("--param", "=5"));
    EXPECT_FALSE(with("--param", "quote_size=5"));  // the market maker's, not the imbalance taker's
    EXPECT_FALSE(with("--param", "cooldown_ns=1000000000000000001"));
    EXPECT_FALSE(with("--max-position", "-1"));
    EXPECT_FALSE(with("--max-order-quantity", "0"));
    EXPECT_FALSE(with("--max-open-orders", "0"));
    EXPECT_FALSE(with("--order-latency-us", "1000000001"));  // ns would overflow past this
    EXPECT_FALSE(with("--taker-fee-ticks", "-30"));
    EXPECT_FALSE(with("--maker-fee-ticks", "-1000001"));
    EXPECT_FALSE(with("--maker-fee-ticks", "+5"));
    EXPECT_TRUE(with("--param", "threshold=1"));
    EXPECT_TRUE(with("--param", "threshold=4"));
    EXPECT_TRUE(with("--max-position", "0"));
}

TEST(ReplayArgsTest, ParsesMarketMakerOptions)
{
    const Parsed p = parse({"backtest",
                            "day.bin",
                            "--symbol",
                            "SPY",
                            "--date",
                            "2019-12-30",
                            "--strategy",
                            "market_maker",
                            "--param",
                            "quote_size=200",
                            "--param",
                            "max_position=800",
                            "--param",
                            "requote_cooldown_ns=250000000",
                            "--maker-fee-ticks",
                            "-20",
                            "--taker-fee-ticks",
                            "30",
                            "--max-open-orders",
                            "4"});

    ASSERT_TRUE(p.ok) << p.error;
    const BacktestArgs& b = p.args.backtest;
    EXPECT_EQ(b.strategy, "market_maker");
    EXPECT_EQ(param(b, "quote_size"), 200);
    EXPECT_EQ(param(b, "max_position"), 800);
    EXPECT_EQ(param(b, "requote_cooldown_ns"), 250'000'000);
    EXPECT_EQ(param(b, "improve_ticks"), 0);  // its default
    EXPECT_EQ(b.limits.max_position, 800);    // from the params
    EXPECT_EQ(b.limits.max_order_quantity.value(), 200u);
    EXPECT_EQ(b.limits.max_open_orders, 4u);  // the flag
    EXPECT_EQ(b.maker_fee_ticks, -20);        // a rebate
    EXPECT_EQ(b.taker_fee_ticks, 30u);
    // A ParamList is sorted by name.
    ASSERT_EQ(b.params.size(), 4u);
    EXPECT_EQ(b.params[0].name, "improve_ticks");
    EXPECT_EQ(b.params[3].name, "requote_cooldown_ns");
}

TEST(ReplayArgsTest, AnUnknownStrategyOrParamNamesWhatIsRegistered)
{
    const Parsed mm = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "mm"});
    EXPECT_FALSE(mm.ok);
    EXPECT_EQ(mm.error, "unknown --strategy 'mm': registered are imbalance market_maker orb trend_taker");
    const Parsed bad = parse(
        {"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "orb", "--param", "x=1"});
    EXPECT_EQ(bad.error, "orb has no param 'x'; its params: range_minutes clip");
    const Parsed range = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--strategy", "orb",
                                "--param", "range_minutes=391"});
    EXPECT_EQ(range.error, "bad value for --param range_minutes: '391' (an integer from 1 to 390)");
    const std::string help = strategies_help();
    for (const StrategyInfo& s : registered_strategies()) {
        EXPECT_NE(help.find("  " + s.name + ": "), std::string::npos) << s.name;
    }
}

// The ORB: its range within the session, and a clip it could enter with.
TEST(ReplayArgsTest, ParsesTheOrbAndRefusesARangeOrClipItCouldNotTradeWith)
{
    auto orb = [](std::initializer_list<std::string_view> more) {
        std::vector<std::string_view> args{"backtest", "day.bin",    "--symbol",   "SPY",
                                           "--date",   "2019-12-30", "--strategy", "orb"};
        args.insert(args.end(), more);
        return parse(args);
    };
    const Parsed p = orb({"--param", "range_minutes=30", "--param", "clip=50"});
    ASSERT_TRUE(p.ok) << p.error;
    EXPECT_EQ(p.args.backtest.strategy, "orb");
    EXPECT_EQ(param(p.args.backtest, "range_minutes"), 30);
    EXPECT_EQ(param(p.args.backtest, "clip"), 50);
    EXPECT_EQ(param(orb({}).args.backtest, "range_minutes"), 15);
    EXPECT_EQ(orb({}).args.backtest.limits.max_open_orders, 1u);
    EXPECT_TRUE(orb({"--param", "range_minutes=1"}).ok);
    EXPECT_TRUE(orb({"--param", "range_minutes=390"}).ok);
    EXPECT_FALSE(orb({"--param", "range_minutes=0"}).ok);
    EXPECT_FALSE(orb({"--param", "range_minutes=391"}).ok);
    const Parsed too_big = orb({"--param", "clip=600", "--max-order-quantity", "600"});  // over the position limit
    EXPECT_FALSE(too_big.ok);
    EXPECT_EQ(too_big.error, "--param clip is over a risk limit: orb could never trade");
    EXPECT_TRUE(orb({"--param", "clip=600", "--max-position", "600"}).ok);
}

TEST(ReplayArgsTest, ParsesASymbolListOrAll)
{
    const Parsed list = parse({"replay", "day.bin", "--symbols", "AAPL,MSFT,SPY", "--date", "2019-12-30"});
    ASSERT_TRUE(list.ok) << list.error;
    EXPECT_EQ(list.args.symbols, (std::vector<std::string_view>{"AAPL", "MSFT", "SPY"}));
    EXPECT_TRUE(list.args.symbol.empty());
    EXPECT_FALSE(list.args.all_symbols);
    EXPECT_EQ(list.args.capacity, default_books_capacity);  // the shared pool of every book

    const Parsed one = parse({"replay", "day.bin", "--symbols", "AAPL", "--date", "2019-12-30"});
    ASSERT_TRUE(one.ok) << one.error;
    EXPECT_EQ(one.args.symbols, (std::vector<std::string_view>{"AAPL"}));

    const Parsed all = parse({"replay", "day.bin", "--all", "--date", "2019-12-30", "--capacity", "4096"});
    ASSERT_TRUE(all.ok) << all.error;
    EXPECT_TRUE(all.args.all_symbols);
    EXPECT_TRUE(all.args.symbols.empty());
    EXPECT_EQ(all.args.capacity, 4'096u);  // an explicit --capacity wins

    auto symbols = [](std::string_view value) {
        return parse({"replay", "day.bin", "--symbols", value, "--date", "2019-12-30"}).ok;
    };
    EXPECT_FALSE(symbols(""));
    EXPECT_FALSE(symbols(","));
    EXPECT_FALSE(symbols("AAPL,"));
    EXPECT_FALSE(symbols(",AAPL"));
    EXPECT_FALSE(symbols("AAPL,,MSFT"));
    EXPECT_FALSE(symbols("AAPL,TOOLONGSYM"));
}

TEST(ReplayArgsTest, SymbolSelectionsAreMutuallyExclusive)
{
    auto error = [](std::vector<std::string_view> argv) {
        const Parsed p = parse(std::move(argv));
        EXPECT_FALSE(p.ok);
        return p.error;
    };

    for (const std::string& e :
         {error({"replay", "day.bin", "--symbol", "AAPL", "--symbols", "MSFT", "--date", "2019-12-30"}),
          error({"replay", "day.bin", "--symbol", "AAPL", "--all", "--date", "2019-12-30"}),
          error({"replay", "day.bin", "--symbols", "MSFT", "--all", "--date", "2019-12-30"})}) {
        EXPECT_NE(e.find("mutually exclusive"), std::string::npos) << e;
    }

    const std::string missing = error({"replay", "day.bin", "--date", "2019-12-30"});
    EXPECT_NE(missing.find("--symbols"), std::string::npos) << missing;
    EXPECT_NE(missing.find("--all"), std::string::npos) << missing;
}

TEST(ReplayArgsTest, BacktestTakesOnlyOneSymbol)
{
    for (const std::vector<std::string_view>& argv :
         {std::vector<std::string_view>{"backtest", "day.bin", "--symbols", "AAPL,MSFT", "--date", "2019-12-30"},
          std::vector<std::string_view>{"backtest", "day.bin", "--all", "--date", "2019-12-30"}}) {
        const Parsed p = parse(argv);
        EXPECT_FALSE(p.ok);
        EXPECT_NE(p.error.find("backtest"), std::string::npos) << p.error;
        EXPECT_NE(p.error.find("--symbol"), std::string::npos) << p.error;
    }
}

TEST(ReplayArgsTest, StoreReadsOneSymbolInPlaceOfTheDayFile)
{
    for (const std::string_view command : {"replay", "backtest"}) {
        const Parsed p = parse({command, "--store", "data/itch/store", "--symbol", "AAPL", "--date", "2019-12-30"});
        ASSERT_TRUE(p.ok) << p.error;
        EXPECT_EQ(p.args.store, "data/itch/store");
        EXPECT_TRUE(p.args.file.empty());
    }
    auto error = [](std::vector<std::string_view> argv) {
        const Parsed p = parse(std::move(argv));
        EXPECT_FALSE(p.ok);
        return p.error;
    };
    EXPECT_EQ(error({"replay", "day.bin", "--store", "s", "--symbol", "AAPL", "--date", "2019-12-30"}),
              "--store replaces <itch-file>: give one");
    EXPECT_EQ(error({"replay", "--store", "s", "--all", "--date", "2019-12-30"}),
              "--store reads one symbol: use --symbol");
    EXPECT_EQ(error({"replay", "--store", "s", "--symbols", "A,B", "--date", "2019-12-30"}),
              "--store reads one symbol: use --symbol");
    EXPECT_EQ(error({"replay", "--store", "", "--symbol", "AAPL", "--date", "2019-12-30"}),
              "bad value for --store: ''");
}

TEST(ReplayArgsTest, ParsesLatencyForReplayOnlyAndJsonForBoth)
{
    const Parsed off = parse({"replay", "day.bin", "--all", "--date", "2019-12-30"});
    ASSERT_TRUE(off.ok) << off.error;
    EXPECT_EQ(off.args.latency_batch, 0u);
    EXPECT_TRUE(off.args.json.empty());

    const Parsed on = parse({"replay", "day.bin", "--all", "--date", "2019-12-30", "--latency", "--json", "out.json"});
    ASSERT_TRUE(on.ok) << on.error;
    EXPECT_EQ(on.args.latency_batch, 1'024u);
    EXPECT_EQ(on.args.json, "out.json");

    // --latency-batch implies --latency, in either order.
    for (const std::vector<std::string_view>& argv :
         {std::vector<std::string_view>{"replay", "day.bin", "--all", "--date", "2019-12-30", "--latency-batch", "64"},
          std::vector<std::string_view>{"replay", "day.bin", "--all", "--date", "2019-12-30", "--latency-batch", "64",
                                        "--latency"}}) {
        const Parsed p = parse(argv);
        ASSERT_TRUE(p.ok) << p.error;
        EXPECT_EQ(p.args.latency_batch, 64u);
    }

    EXPECT_FALSE(parse({"replay", "day.bin", "--all", "--date", "2019-12-30", "--latency-batch", "0"}).ok);
    const Parsed latency = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--latency"});
    EXPECT_FALSE(latency.ok);
    EXPECT_NE(latency.error.find("unknown backtest option --latency"), std::string::npos) << latency.error;
    EXPECT_FALSE(parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--latency-batch", "64"}).ok);

    const Parsed json = parse({"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--json", "a.json"});
    ASSERT_TRUE(json.ok) << json.error;
    EXPECT_EQ(json.args.json, "a.json");
}

TEST(ReplayArgsTest, HelpAndVersionWinAnywhere)
{
    for (const std::vector<std::string_view>& argv :
         {std::vector<std::string_view>{"--help"}, std::vector<std::string_view>{"-h"},
          std::vector<std::string_view>{"replay", "--help"},
          std::vector<std::string_view>{"backtest", "day.bin", "--param", "clip=0", "-h"},
          std::vector<std::string_view>{"play", "--help"}}) {
        const Parsed p = parse(argv);
        ASSERT_TRUE(p.ok) << p.error;
        EXPECT_EQ(p.args.command, CliCommand::Help);
    }
    for (const std::vector<std::string_view>& argv :
         {std::vector<std::string_view>{"--version"},
          std::vector<std::string_view>{"replay", "day.bin", "--version"}}) {
        const Parsed p = parse(argv);
        ASSERT_TRUE(p.ok) << p.error;
        EXPECT_EQ(p.args.command, CliCommand::Version);
    }
}

TEST(ReplayArgsTest, ParsesLatencyInNanoseconds)
{
    auto backtest = [](std::vector<std::string_view> flags) {
        std::vector<std::string_view> argv{"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"};
        argv.insert(argv.end(), flags.begin(), flags.end());
        return parse(argv);
    };

    const Parsed ns = backtest({"--order-latency-ns", "300", "--report-latency-ns", "0"});
    ASSERT_TRUE(ns.ok) << ns.error;
    EXPECT_EQ(ns.args.backtest.order_latency_ns, 300u);
    EXPECT_EQ(ns.args.backtest.report_latency_ns, 0u);

    const Parsed last = backtest({"--order-latency-us", "5", "--order-latency-ns", "300"});
    ASSERT_TRUE(last.ok) << last.error;
    EXPECT_EQ(last.args.backtest.order_latency_ns, 300u);  // the last flag wins

    const Parsed us = backtest({"--report-latency-us", "7"});
    ASSERT_TRUE(us.ok) << us.error;
    EXPECT_EQ(us.args.backtest.report_latency_ns, 7'000u);

    EXPECT_TRUE(backtest({"--order-latency-ns", "1000000000000"}).ok);  // 1,000 s, as for -us
    EXPECT_FALSE(backtest({"--order-latency-ns", "1000000000001"}).ok);
    EXPECT_FALSE(backtest({"--report-latency-ns", "-1"}).ok);
    EXPECT_FALSE(
        parse({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--order-latency-ns", "300"}).ok);
}

TEST(ReplayArgsTest, ErrorsNameTheCommandAndOption)
{
    auto error = [](std::vector<std::string_view> argv) {
        const Parsed p = parse(std::move(argv));
        EXPECT_FALSE(p.ok);
        return p.error;
    };

    const std::string clip = error({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--clip", "100"});
    EXPECT_NE(clip.find("unknown replay option --clip"), std::string::npos) << clip;

    // Unknown, not "needs a value", even as the last argument.
    const std::string last = error({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--frobnicate"});
    EXPECT_NE(last.find("unknown replay option --frobnicate"), std::string::npos) << last;
    EXPECT_EQ(last.find("needs a value"), std::string::npos) << last;

    const std::string value = error({"replay", "day.bin", "--date", "2019-12-30", "--symbol"});
    EXPECT_NE(value.find("--symbol needs a value"), std::string::npos) << value;

    const std::string command = error({"play", "day.bin"});
    EXPECT_NE(command.find("'play'"), std::string::npos) << command;

    const std::string none = error({});
    EXPECT_NE(none.find("missing"), std::string::npos) << none;

    const std::string depth =
        error({"replay", "day.bin", "--symbol", "SPY", "--date", "2019-12-30", "--depth", "five"});
    EXPECT_NE(depth.find("--depth"), std::string::npos) << depth;
    EXPECT_NE(depth.find("'five'"), std::string::npos) << depth;
}

// An example of every flag: each parses, and each appears in the help as
// " --flag ". A flag added to the parser needs a row here.
TEST(ReplayArgsTest, UsageDocumentsEveryOption)
{
    using Argv = std::vector<std::string_view>;
    auto replay = [](Argv flags) {
        Argv argv{"replay", "day.bin", "--all", "--date", "2019-12-30"};
        argv.insert(argv.end(), flags.begin(), flags.end());
        return argv;
    };
    auto backtest = [](Argv flags) {
        Argv argv{"backtest", "day.bin", "--symbol", "SPY", "--date", "2019-12-30"};
        argv.insert(argv.end(), flags.begin(), flags.end());
        return argv;
    };

    const std::vector<Argv> examples{
        {"replay", "day.bin", "--symbol", "AAPL", "--date", "2019-12-30"},
        {"replay", "day.bin", "--symbols", "AAPL,MSFT", "--date", "2019-12-30"},
        {"replay", "--store", "store", "--symbol", "AAPL", "--date", "2019-12-30"},
        replay({"--utc-offset", "-04:00"}),
        replay({"--allow-truncated-tail"}),
        replay({"--depth", "10"}),
        replay({"--check-invariants", "1000"}),
        replay({"--capacity", "4096"}),
        replay({"--book", "ladder"}),
        backtest({"--book", "list"}),
        replay({"--threads", "2"}),
        backtest({"--threads", "2"}),
        replay({"--latency"}),
        replay({"--latency-batch", "64"}),
        replay({"--json", "out.json"}),
        backtest({"--json", "out.json"}),
        backtest({"--strategy", "market_maker"}),
        backtest({"--order-latency-ns", "300"}),
        backtest({"--order-latency-us", "20"}),
        backtest({"--report-latency-ns", "300"}),
        backtest({"--report-latency-us", "20"}),
        backtest({"--param", "clip=200"}),
        backtest({"--max-position", "10"}),
        backtest({"--max-order-quantity", "50"}),
        backtest({"--max-open-orders", "3"}),
        backtest({"--taker-fee-ticks", "30"}),
        backtest({"--maker-fee-ticks", "-20"}),
        {"--help"},
        {"--version"},
    };

    for (const Argv& argv : examples) {
        const Parsed p = parse(argv);
        EXPECT_TRUE(p.ok) << argv[0] << " " << argv.back() << ": " << p.error;
        for (const std::string_view token : argv) {
            if (token.starts_with("--")) {
                EXPECT_NE(usage.find(" " + std::string{token} + " "), std::string_view::npos) << token;
            }
        }
    }
    EXPECT_NE(usage.find("-h, --help"), std::string_view::npos);
    EXPECT_NE(usage.find("exit status"), std::string_view::npos);

    ASSERT_TRUE(usage.ends_with('\n'));
    for (std::size_t start = 0; start < usage.size();) {
        const std::size_t end = usage.find('\n', start);
        EXPECT_LE(end - start, 100u) << usage.substr(start, end - start);
        start = end + 1;
    }
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
