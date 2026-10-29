#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "chronos/server/script_strategy.hpp"
#include "simulator/engine/event_loop.hpp"
#include "simulator/market/market_replay.hpp"
#include "simulator/market/vector_source.hpp"
#include "support/synthetic_market.hpp"

namespace simulator::server {
namespace {

constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

// The imbalance taker of include/simulator/strategies/imbalance_taker.hpp, line for line in Lua.
constexpr std::string_view kImbalance = R"lua(
summary = "The imbalance taker, in Lua"
local threshold = param("threshold", 3.0, {min = 1})
local clip = param("clip", 100, {min = 1})
local max_position = param("max_position", 500)
local cooldown = param("cooldown_ns", 100000000)
limits{max_position = max_position, max_order_quantity = clip, max_open_orders = 1}

local ready_at, sent, rejects, fills = 0, 0, 0, 0

local function send(side, price)
  ready_at = now() + cooldown
  local id = (side == "buy" and buy or sell)(price, clip, "ioc")
  if id then sent = sent + 1 else rejects = rejects + 1 end
  stat("Orders sent", sent)
  stat("Risk rejects", rejects)
end

function on_market(e)
  if not trading() or now() < ready_at then return end
  local bid, bid_size = best_bid()
  local ask, ask_size = best_ask()
  if not bid or not ask then return end
  local pos = position()
  if bid_size >= threshold * ask_size and pos + clip <= max_position then
    send("buy", ask)
  elseif ask_size >= threshold * bid_size and pos - clip >= -max_position then
    send("sell", bid)
  end
end

function on_report(r)
  if r.type == "fill" then
    fills = fills + 1
    stat("Fills", fills)
  end
end
)lua";

struct Outcome
{
    LoopReport loop{};
    std::uint64_t book_digest{0};
    std::uint64_t fills_digest{0};
    std::int64_t pnl{0};
    ExecutionStatistics execution{};

    bool operator==(const Outcome&) const = default;
};

Outcome run(AnyStrategy& strategy, const std::vector<MarketEvent>& events, const RiskLimits& limits)
{
    VectorSource source{events};
    MarketBook book{0, 4'096};
    ExecutionSimulator execution;
    Portfolio portfolio{kEquity, limits, FeeSchedule{.taker_fee_per_share = 30}};
    EventLoop<AnyStrategy> loop{source,    book,     execution,
                                portfolio, strategy, {.order_latency = 300, .report_latency = 300}};
    return {.loop = loop.run(),
            .book_digest = book.digest(),
            .fills_digest = portfolio.fills_digest(),
            .pnl = portfolio.pnl(),
            .execution = execution.statistics()};
}

// The params a script declares, at their defaults but for `given`.
ParamList params_of(std::string_view source, std::vector<Param> given = {})
{
    const ScriptInfo info = describe_script(source);
    EXPECT_EQ(info.error, "");
    ParamList params;
    std::string bad;
    EXPECT_TRUE(resolve_params(info.params, given, params, bad)) << bad;
    return params;
}

std::unique_ptr<AnyStrategy> script(std::string_view source, std::vector<Param> given = {})
{
    const DaySession day{.open = 0, .close = 1'000'000'000'000};
    static const std::vector<DaySession> sessions{day};
    return make_script_strategy(source, params_of(source, std::move(given)), {.sessions = sessions});
}

// A one-callback script whose on_market runs `body`, over a short synthetic market; the error it stopped on, if any.
std::string error_of(std::string_view body)
{
    const std::string source = "function on_market(e)\n" + std::string{body} + "\nend\n";
    const std::unique_ptr<AnyStrategy> s = script(source);
    run(*s, synthetic_market(3, 200), {.max_position = 100, .max_order_quantity = Quantity{100}, .max_open_orders = 1});
    return std::string{s->error()};
}

TEST(ScriptStrategyTest, TheImbalanceTakerInLuaMakesTheSameFillsAsTheCppPlugin)
{
    const std::vector<MarketEvent> events = synthetic_market(11, 20'000);
    const RiskLimits limits{.max_position = 500, .max_order_quantity = Quantity{100}, .max_open_orders = 1};
    ParamList params;
    std::string bad;
    const StrategyInfo& cpp = *find_strategy("imbalance");
    ASSERT_TRUE(resolve_params(cpp.params, std::vector<Param>{{"cooldown_ns", 400}}, params, bad));
    const std::unique_ptr<AnyStrategy> native = cpp.make(params, {});
    const std::unique_ptr<AnyStrategy> lua = script(kImbalance, {{"cooldown_ns", 400}});

    const Outcome expected = run(*native, events, limits);
    const Outcome got = run(*lua, events, limits);
    ASSERT_EQ(lua->error(), "");
    EXPECT_GT(expected.execution.taker_fills, 100u);
    EXPECT_EQ(got, expected);
    std::vector<Stat> stats;
    lua->stats(stats);
    std::vector<Stat> native_stats;
    native->stats(native_stats);
    ASSERT_EQ(stats.size(), 3u);
    EXPECT_EQ(stats[0], native_stats[0]);  // Orders sent
    EXPECT_EQ(stats[1], native_stats[1]);  // Risk rejects
    EXPECT_EQ(stats[2], native_stats[2]);  // Fills

    // Another run of the script is the same, and the history is the plain replay's.
    EXPECT_EQ(run(*script(kImbalance, {{"cooldown_ns", 400}}), events, limits), got);
    VectorSource plain{events};
    MarketBook history{0, 4'096};
    MarketReplay::run(plain, history, {});
    EXPECT_EQ(got.book_digest, history.digest());
}

TEST(ScriptStrategyTest, DescribesItsParamsLimitsAndSummary)
{
    const ScriptInfo info = describe_script(kImbalance);
    ASSERT_EQ(info.error, "");
    EXPECT_EQ(info.summary, "The imbalance taker, in Lua");
    ASSERT_EQ(info.params.size(), 4u);
    EXPECT_EQ(
        info.params[0],
        (ParamInfo{
            .name = "threshold", .default_value = 3.0, .min = 1.0, .max = std::numeric_limits<double>::infinity()}));
    EXPECT_EQ(info.params[1],
              (ParamInfo{.name = "clip",
                         .default_value = std::int64_t{100},
                         .min = std::int64_t{1},
                         .max = std::int64_t{1'000'000'000}}));
    EXPECT_EQ(info.limits.max_position, 500);
    EXPECT_EQ(info.limits.max_order_quantity, Quantity{100});
    EXPECT_EQ(info.limits.max_open_orders, 1u);

    const ScriptInfo plain = describe_script("function on_timer(id) end");
    EXPECT_EQ(plain.error, "");
    EXPECT_TRUE(plain.params.empty());
    EXPECT_EQ(plain.limits.max_open_orders, 10u);  // a script's limits until it declares its own
    const ScriptInfo help = describe_script(R"(local x = param("x", 2, {min = -5, max = 5, help = "a number"})
function on_timer() end)");
    EXPECT_EQ(help.params.at(0).min, ParamValue{std::int64_t{-5}});
    EXPECT_EQ(help.params.at(0).help, "a number");
}

TEST(ScriptStrategyTest, ARunTakesItsParamsInPlaceOfTheDefaults)
{
    const std::string source = R"(local size = param("size", 7)
local ratio = param("ratio", 0.5)
function on_market(e) stat("size", size) stat("ratio", ratio) end)";
    const std::unique_ptr<AnyStrategy> s = script(source, {{"size", 9}});
    run(*s, synthetic_market(3, 10), {.max_position = 1, .max_order_quantity = Quantity{1}, .max_open_orders = 1});
    std::vector<Stat> stats;
    s->stats(stats);
    EXPECT_EQ(stats, (std::vector<Stat>{{"size", 9}, {"ratio", 0.5}}));
}

TEST(ScriptStrategyTest, ABadScriptSaysWhyAndWhere)
{
    EXPECT_EQ(describe_script("x = = 1").error, "load: script:1: unexpected symbol near '='");
    EXPECT_EQ(describe_script("x = 1").error, "load: the script defines none of on_market, on_report and on_timer");
    EXPECT_EQ(describe_script("on_market = 5").error, "load: on_market is a number, not a function");
    EXPECT_EQ(describe_script("buy(100, 1)\nfunction on_timer() end").error,
              "load: script:1: buy() works only inside on_market, on_report or on_timer");
    EXPECT_EQ(describe_script("param('a', 1) param('a', 2) function on_timer() end").error,
              "load: script:1: param 'a' is declared twice");
    EXPECT_EQ(describe_script("param('a', 5, {max = 3}) function on_timer() end").error,
              "load: script:1: param 'a': needs min <= default <= max, min finite");
    EXPECT_EQ(describe_script("param('A', 1) function on_timer() end").error,
              "load: script:1: param 'A': a name is 1-40 of a-z, 0-9 and _");
    EXPECT_EQ(describe_script("param('a', 1, {max = 2.5}) function on_timer() end").error,
              "load: script:1: param 'a': max must be an integer");
    EXPECT_EQ(describe_script("limits{max_open_orders = 0} function on_timer() end").error,
              "load: script:1: limits: max_open_orders must be an integer from 1 to 1000000000");
    EXPECT_EQ(describe_script(std::string(max_script_bytes + 1, ' ')).error, "load: the script is over 48 KiB");
    EXPECT_EQ(describe_script("\x1bLua").error.substr(0, 6), "load: ");  // bytecode is refused
}

TEST(ScriptStrategyTest, AnErrorInACallbackStopsTheStrategyWithItsLine)
{
    EXPECT_EQ(error_of("local x = nil\nx.y = 1"), "on_market: script:3: attempt to index a nil value (local 'x')");
    EXPECT_EQ(error_of("buy(0, 1)"), "on_market: script:2: bad argument #1 to 'buy' (a positive price in ticks)");
    EXPECT_EQ(error_of("buy(100, 1, 'fok')"), "on_market: script:2: bad argument #3 to 'buy' ('ioc' or 'post_only')");
    EXPECT_EQ(error_of("param('a', 1)"), "on_market: script:2: param() works only at the top level of the script");
    EXPECT_EQ(error_of("level('up', 1)"), "on_market: script:2: bad argument #1 to 'level' ('buy' or 'sell')");
    EXPECT_EQ(error_of("error('mine')"), "on_market: script:2: mine");
    EXPECT_EQ(error_of("error({})"), "on_market: an error that is not a string");
}

TEST(ScriptStrategyTest, TheSandboxHasNoFilesClockLoaderOrCollector)
{
    for (const char* missing :
         {"io", "os", "require", "load", "loadfile", "dofile", "collectgarbage", "debug", "package"}) {
        SCOPED_TRACE(missing);
        EXPECT_EQ(describe_script(std::string{"assert("} + missing + " == nil) function on_timer() end").error, "");
    }
    EXPECT_NE(error_of("math.randomseed()"), "");  // a seed of the script's own, never the clock's
    EXPECT_EQ(error_of("math.randomseed(7) local x = math.random(10)"), "");
}

// String hashes, math.random and table.sort's pivot start from fixed seeds, so table order, random draws and the order
// a sort leaves equal elements in repeat from run to run.
TEST(ScriptStrategyTest, TableOrderAndRandomDrawsRepeatAcrossRuns)
{
    const std::string source = R"(local t = {}
for i = 1, 200 do t["key" .. i] = i end
local order = {}
for k in pairs(t) do order[#order + 1] = k end
local draws = {}
for i = 1, 5 do draws[i] = math.random(1000000) end
local rows = {}
for i = 1, 3000 do rows[i] = {k = i % 3, id = i} end
table.sort(rows, function(a, b) return a.k < b.k end)
local sorted = {}
for i = 1, 20 do sorted[i] = rows[i].id end
function on_market(e)
  log(table.concat(order, ",", 1, 20)) log(table.concat(draws, ",")) log(table.concat(sorted, ","))
  error("done")
end)";
    std::vector<std::string> first;
    for (int i = 0; i < 3; ++i) {
        const std::unique_ptr<AnyStrategy> s = script(source);
        std::vector<char> spacer(static_cast<std::size_t>(i) * 4'096);  // other addresses for the next state
        run(*s, synthetic_market(3, 10), {.max_position = 1, .max_order_quantity = Quantity{1}, .max_open_orders = 1});
        const std::vector<std::string> lines{s->log().begin(), s->log().end()};
        ASSERT_EQ(lines.size(), 3u);
        if (i == 0) {
            first = lines;
        }
        EXPECT_EQ(lines, first);
    }
}

TEST(ScriptStrategyTest, AnEndlessLoopOrRunawayMemoryStopsAtTheSamePlaceEveryTime)
{
    EXPECT_EQ(error_of("while true do end"),
              "on_market: script:2: over 10 million instructions in one call: an endless loop?");
    EXPECT_EQ(describe_script("while true do end function on_timer() end").error,
              "load: script:1: over 10 million instructions in one call: an endless loop?");
    const std::string grow = "local x = string.rep('x', 1 << 16) local t = {} for i = 1, 1e9 do t[i] = x .. i end";
    EXPECT_EQ(error_of(grow), "on_market: not enough memory");
}

// A call over its budget stays over it: every instruction raises, so a pcall cannot catch the error and go on.
TEST(ScriptStrategyTest, APcallCannotCatchTheBudgetAndXpcallAndFinalizersAreGone)
{
    EXPECT_EQ(error_of("while true do pcall(function() while true do end end) end"),
              "on_market: script:2: over 10 million instructions in one call: an endless loop?");
    EXPECT_EQ(error_of("local ok = pcall(function() while true do end end)\nlog(ok)"),
              "on_market: script:3: over 10 million instructions in one call: an endless loop?");
    EXPECT_EQ(describe_script("assert(xpcall == nil) function on_timer() end").error, "");
    EXPECT_EQ(
        describe_script("setmetatable({}, {__gc = function() while true do end end}) function on_timer() end").error,
        "load: script:1: setmetatable: a metatable with __gc is not allowed");
    EXPECT_EQ(
        describe_script("local t = setmetatable({}, {__index = {x = 1}}) assert(t.x == 1) function on_timer() end")
            .error,
        "");
}

// What Lua's C library would do out of the budget's sight is gone, bounded, or done in Lua.
TEST(ScriptStrategyTest, TheCLibraryCannotRunUncounted)
{
    EXPECT_EQ(describe_script(R"(
assert(string.match == nil and string.gmatch == nil and string.gsub == nil and table.move == nil)
assert(("a.b"):find(".") == 2)  -- plain text, no pattern
local t = {5, 1, 4, 2, 3}
table.sort(t)
assert(table.concat(t, ",") == "1,2,3,4,5")
table.sort(t, function(a, b) return a > b end)
assert(table.concat(t, ",") == "5,4,3,2,1")
table.insert(t, 1, 9) table.insert(t, 7) table.insert(t, 3, 8)
assert(table.concat(t, ",") == "9,5,8,4,3,2,1,7")
assert(table.remove(t, 1) == 9 and table.remove(t) == 7 and table.remove(t, 2) == 8)
assert(table.concat(t, ",") == "5,4,3,2,1")
assert(not pcall(table.insert, t, 9, 1) and not pcall(table.remove, t, 9))
function on_timer() end)")
                  .error,
              "");
    EXPECT_EQ(error_of("string.find('x', string.rep('a', 257))"),
              "on_market: script:2: bad argument #2 to 'find' (at most 256 bytes)");
    EXPECT_EQ(error_of("string.rep('', 1 << 40)"),
              "on_market: script:2: bad argument #2 to 'rep' (at most 16777216 copies)");
    // Large copies and a budget mostly spent beforehand, so neither takes long under a sanitizer.
    EXPECT_EQ(error_of("local x = string.rep('x', 1 << 16) for i = 1, 1000 do local s = string.rep(x, 160) end"),
              "on_market: script:2: over 1 GiB allocated in one call");
    const std::string sorted =
        error_of("local t = {} for i = 1, 1e5 do t[i] = i % 7 end for i = 1, 9e6 do end table.sort(t)");
    EXPECT_EQ(sorted, "on_market: table:3: over 10 million instructions in one call: an endless loop?");  // counted
}

// An address changes from run to run, so neither tostring nor string.format shows one.
TEST(ScriptStrategyTest, NoAddressReachesAScript)
{
    EXPECT_EQ(describe_script(R"(
assert(tostring({}) == "table" and tostring(print) == "function")
assert(tostring(setmetatable({}, {__tostring = function() return "mine" end})) == "mine")
assert(string.format("%s %d", {}, 7) == "table 7")
assert(not pcall(string.format, "%5p", {}))
assert(string.format("100%%") == "100%")
function on_timer() end)")
                  .error,
              "");
}

// What a script may ask of the engine in one call is bounded: a timer only after now, 64 pending, 256 orders and
// cancels, 32 params; a long error is cut.
TEST(ScriptStrategyTest, TimersOrdersCancelsParamsAndErrorsAreBounded)
{
    const std::unique_ptr<AnyStrategy> timers = script(R"(local done = false
function on_market(e)
  if done then return end
  done = true
  local set = 0
  assert(not set_timer(now(), 1) and not set_timer(now() - 1, 1))
  for i = 1, 100 do if set_timer(now() + i, i) then set = set + 1 end end
  log("set", set)
end
function on_timer(id) log("timer", id) end)");
    run(*timers, synthetic_market(3, 2'000),
        {.max_position = 1, .max_order_quantity = Quantity{1}, .max_open_orders = 1});
    ASSERT_EQ(timers->error(), "");
    EXPECT_EQ(timers->log()[0], "set 64");
    EXPECT_EQ(timers->log().size(), 65u);  // and each of the 64 fired

    EXPECT_EQ(error_of("for i = 1, 300 do cancel(i) end"),
              "on_market: script:2: over 256 orders and cancels in one call");
    std::string params;
    for (int i = 0; i < 33; ++i) {
        params += "param('p" + std::to_string(i) + "', 1) ";
    }
    EXPECT_EQ(describe_script(params + "function on_timer() end").error, "load: script:1: param: at most 32 params");
    const std::string long_error = error_of("error(string.rep('x', 10000), 0)");
    EXPECT_EQ(long_error.size(), std::string{"on_market: "}.size() + 300);
    EXPECT_EQ(error_of(R"(error(string.rep('\u{e9}', 400), 0))").size() % 2, 1u);  // "on_market: " and whole é's
}

TEST(ScriptStrategyTest, LogsAndStatsAreKeptWithinTheirCaps)
{
    const std::unique_ptr<AnyStrategy> s = script(R"(local n = 0
function on_market(e)
  n = n + 1
  log("event", n, e.type, string.rep("y", 400))
  stat("price", 1234500, true)
end)");
    run(*s, synthetic_market(3, 300), {.max_position = 1, .max_order_quantity = Quantity{1}, .max_open_orders = 1});
    ASSERT_EQ(s->log().size(), max_script_log_lines);
    EXPECT_EQ(s->log()[0].substr(0, 14), "event 1 add yy");
    EXPECT_EQ(s->log()[0].size(), 300u);  // cut at 300 characters
    std::vector<Stat> stats;
    s->stats(stats);
    EXPECT_EQ(stats, (std::vector<Stat>{{"price", 1'234'500, true}}));
}

TEST(ScriptStrategyTest, ReadsTheBookTheEventTheReportsAndTimers)
{
    // Posts one quote a tick under the best bid, cancels it at a timer, and records what it saw.
    const std::unique_ptr<AnyStrategy> s = script(R"(local id
function on_market(e)
  local bid, size, orders = best_bid()
  if id == nil and bid then
    local p2 = level("buy", 1)
    assert(p2 == bid and size > 0 and orders > 0)
    id = buy(bid - 100, 10, "post_only")
    assert(set_timer(now() + 1000, 7))
    assert(dollars(ticks(99.99)) == 99.99 and price_scale == 4)
    local open, close = session()
    assert(open == 0 and close == 1000000000000)
  end
end
function on_report(r)
  log(r.type, r.id == id, r.side, r.price, r.qty, r.leaves, r.reason)
end
function on_timer(t)
  log("timer", t, open_orders())
  cancel(id)
end)");
    run(*s, synthetic_market(5, 2'000),
        {.max_position = 100, .max_order_quantity = Quantity{100}, .max_open_orders = 1});
    ASSERT_EQ(s->error(), "");
    const std::vector<std::string> lines{s->log().begin(), s->log().end()};
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines[0], "accepted true buy 0 10 10 none");  // an acceptance has no price; a fill does
    EXPECT_EQ(lines[1], "timer 7 1");  // resting at the timer, which sends the cancel
    EXPECT_EQ(lines[2].substr(0, 14), "fill true buy ");               // filled while the cancel travelled
    EXPECT_EQ(lines.back(), "cancel_rejected true buy 0 0 0 none");  // so it arrived too late
}

TEST(ScriptStrategyTest, TheStoreSavesListsReadsAndRemovesScripts)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "chronos-script-store-test";
    std::filesystem::remove_all(dir);
    ScriptStore store{dir};
    EXPECT_TRUE(store.names().empty());
    std::string error;
    ASSERT_TRUE(store.write("beta", "function on_timer() end", error)) << error;
    ASSERT_TRUE(store.write("alpha", "x = = 1", error)) << error;  // saved though it does not load
    EXPECT_EQ(store.names(), (std::vector<std::string>{"alpha", "beta"}));
    std::string source;
    ASSERT_TRUE(store.read("beta", source));
    EXPECT_EQ(source, "function on_timer() end");
    EXPECT_FALSE(store.read("../beta", source));
    EXPECT_FALSE(store.read("gamma", source));
    EXPECT_TRUE(store.remove("alpha"));
    EXPECT_FALSE(store.remove("alpha"));
    EXPECT_EQ(store.names(), (std::vector<std::string>{"beta"}));
    for (std::size_t i = 1; i < ScriptStore::max_scripts; ++i) {
        ASSERT_TRUE(store.write("s" + std::to_string(i), "", error)) << i;
    }
    EXPECT_FALSE(store.write("one_too_many", "", error));
    EXPECT_EQ(error, "too_many");
    EXPECT_TRUE(store.write("beta", "-- replaced", error));  // replacing one is not a new one
    std::filesystem::remove_all(dir);
}

TEST(ScriptStrategyTest, NamesAScriptsStrategy)
{
    EXPECT_TRUE(is_script_name("script:my_mm"));
    EXPECT_FALSE(is_script_name("script:"));
    EXPECT_FALSE(is_script_name("script:../x"));
    EXPECT_FALSE(is_script_name("my_mm"));
}

}  // namespace
}  // namespace simulator::server
