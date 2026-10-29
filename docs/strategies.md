# Writing a strategy

There are two ways to write one, and both reach the same engine, fill model and checks:

| | A C++ plugin | A script written in the browser |
|---|---|---|
| Where | a file in [`strategies/`](../strategies/) | the Strategies page of `chronos-serve` |
| Language | C++20, against the `Strategy` concept | Lua 5.4, in a sandbox |
| To use a change | rebuild and restart the server | Save |
| Runs in | the CLI's `backtest`, the Strategy Lab, Watch in the terminal | the Strategy Lab, Watch in the terminal |
| Cost an event | none measurable over the engine's own | about 230 ns more (SPY's full day: 762 ms against 272 ms) |
| Guards | the build's checks and `StrategyRegistryTest` | a sandbox with no files or clock; per call, 10 million instructions, 1 GiB allocated and 30 s; 64 MiB |

A script is the quick way to try an idea; a plugin is the way to keep one, test it in C++, and run it at full speed or
from the command line. The same idea gives the same fills either way: a Lua port of the imbalance taker makes exactly
the C++ plugin's 48,643 fills, PnL and digests on SPY's full day.

## Before you write one

These hold for every strategy, and the engine enforces most of them.

1. **It reads history and never changes it.** The book a strategy sees is the historical one, rebuilt from the feed,
   and read-only. Its orders go through the order gateway to the execution simulator, which fills them by the queue
   rules of [backtesting.md](backtesting.md#d2-the-fill-model): a resting order fills only once the historical
   quantity ahead of it at its price has traded or left. Simulated fills never remove historical liquidity, so a
   strategy cannot move the market.
2. **Time is exchange time, and it takes time to act.** An order sent at `t` arrives at `t` plus the order latency
   (50 µs by default); its report, a fill say, reaches the strategy a report latency after the exchange produced it.
   Do not count on a fill until its report arrives; `ctx.portfolio` has booked it by then.
3. **The same input gives the same result.** Every run is reproduced and verified by its digests. Read no clock,
   draw no randomness from the machine (a seeded generator of your own is fine), and touch no file or network.
   `ctx.now()` is the exchange time.
4. **Units are integers.** A price is in ticks of 10^-`price_scale` dollars ($1 is 10,000 ticks for ITCH); at $1 or
   more it must be on the cent grid, or the order is rejected `OffTick`. A quantity is shares; a time is nanoseconds
   since the Unix epoch, UTC.
5. **Risk limits are checked on every send.** `ctx.gateway.send` returns `RiskCheck::Accepted` or why it refused:
   order too large, position limit, too many open orders, or the gateway closed (outside the trading window). A
   strategy's default limits come from its params; a run may override them.
6. **Callbacks are `noexcept` and called often.** `on_market` runs on every event the book applies, millions a day for
   a liquid symbol. Keep it to arithmetic on the book's top and your own state.
7. **Stats are for decisions; PnL is the portfolio's.** Count what the strategy decided (entries, quotes, risk
   rejects); the run already reports PnL, fills, fees and drawdown.

## What a strategy sees and does

A strategy is any class with these three members (the `Strategy` concept in
[`engine/strategy.hpp`](../include/simulator/engine/strategy.hpp)):

| Callback | When |
|---|---|
| `void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept` | after the book applied `event`; `ctx.book` is the book after it |
| `void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept` | a report on one of its orders, after the report latency and after the portfolio booked it |
| `void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept` | a timer it set has come due |

| `ctx.` | Gives |
|---|---|
| `book` | the historical book: `best(side, level)`, `depth(side, levels)`, `trading_state()`, `tick_at(price)` |
| `portfolio` | `position()`, `cash()`, `pnl()`, `realised_pnl()`, `unrealised_pnl()`, `fees()`, `open_orders()` |
| `trade` | in `on_market`, the trade the event printed (price, quantity, side), else null |
| `gateway.send(side, price, quantity, tif, id)` | sends an order: `TimeInForce::Ioc` fills what crosses on arrival, `TimeInForce::Day` is post-only and rests; `id` is set on acceptance |
| `gateway.cancel(id)` | cancels a resting order |
| `now()`, `set_timer(at, id)` | the exchange time; a timer at `at`, no earlier than now |

A report is `Accepted`, `Rejected` (with a reason), `Fill` (price, quantity, `leaves`, maker or taker), `Cancelled`,
`CancelRejected` or `Expired` ([`execution_report.hpp`](../include/simulator/sim/execution_report.hpp)).

## A plugin, step by step

### 1. Copy the example

[`strategies/trend_taker.cpp`](../strategies/trend_taker.cpp) is a complete plugin. Copy it to
`strategies/<your_name>.cpp` and change the four parts:

```cpp
#include "simulator/engine/strategy_registry.hpp"

namespace {  // keeps each plugin's names its own

using namespace simulator;

// 1. The params: each field's default is the param's default.
struct TrendParams
{
    std::uint64_t clip{100};           // shares per entry
    std::int64_t run_length{3};        // rising trades in a row that trigger an entry
    Timestamp hold_ns{1'000'000'000};  // how long a position is held before it is sold
};

// 2. The strategy: the three callbacks, and whatever it counts.
class TrendTaker
{
    // ... see the file
public:
    explicit TrendTaker(const TrendParams& params) noexcept;
    void on_market(StrategyContext& ctx, const MarketEvent& event) noexcept;
    void on_report(StrategyContext& ctx, const ExecutionReport& report) noexcept;
    void on_timer(StrategyContext& ctx, std::uint64_t timer_id) noexcept;
    std::uint64_t entries() const noexcept;
};

// 3. The registration, run once when the program starts.
[[maybe_unused]] const bool registered = register_plugin(StrategyPlugin<TrendTaker, TrendParams>{
    .name = "trend_taker",            // what --strategy and the API call it
    .label = "Trend taker (example)",  // what the Lab shows
    .summary = "An IOC buy after a run of rising trades, sold after a hold",
    .params = {param("clip", &TrendParams::clip, 1, 1'000'000, "shares per entry"),
               param("run_length", &TrendParams::run_length, 1, 1'000, "rising trades that trigger an entry"),
               param("hold_ns", &TrendParams::hold_ns, 1, 3'600'000'000'000, "how long a position is held, in ns")},
    // 4. Its default risk limits, from its params; required.
    .limits = [](const TrendParams& p) {
        return RiskLimits{.max_position = static_cast<std::int64_t>(p.clip),
                          .max_order_quantity = Quantity{p.clip},
                          .max_open_orders = 1};
    },
    // Optional: the counters shown after a run.
    .stats = [](const TrendTaker& s, std::vector<Stat>& out) {
        out = {{"Entries", static_cast<double>(s.entries())}};
    },
});

}  // namespace
```

### 2. Declare it

| Field | Required | What it is |
|---|---|---|
| `name` | yes | 1-40 of `a-z`, `0-9` and `_`, unique |
| `label`, `summary` | yes | the Lab's name for it and one line on what it does |
| `params` | no | `param(name, &Params::field, min, max, help)` for each field the user may set: an integer field takes integers, a `double` field any finite number; `std::numeric_limits<double>::infinity()` as `max` means no cap |
| `limits` | yes | the risk limits a run takes unless told otherwise, from the params |
| `make` | no | `[](const Params& p, const StrategySetup& setup) { return MyStrategy{p, setup.sessions}; }` when the strategy needs each day's regular session or the price scale; otherwise it is built from its params alone |
| `stats` | no | appends `{name, value}` pairs, or `{name, ticks, true}` for a price, which the Lab shows in dollars |
| `check` | no | returns the name of a param that makes a run pointless under its limits (the ORB's `clip` over the position limit), or `""` |

Keep params small and capped: a quantity times a price must stay far from `int64` overflow, which is why the shipped
strategies cap sizes at 10^9.

### 3. Build, and the checks run on it

```bash
cmake --build build -j       # CMake notices the new file and configures again by itself
ctest --test-dir build -R StrategyRegistry
./build/simulator --help     # lists every strategy with its params
```

`StrategyRegistryTest` runs every registered strategy, yours included, with no test written for it: its declaration
is sound, its defaults can trade under its own limits, two runs over a synthetic market are identical, and it leaves
the history digest of a plain replay. A taken or malformed name, a default outside its bounds, or a missing `limits`
stops every program at start-up with a message naming the strategy. `scripts/check-isolation.sh` fails a plugin that
includes server or web code.

### 4. Run it

```bash
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy trend_taker \
    --param run_length=5 --param hold_ns=500000000
```

`--param NAME=VALUE` sets a param (repeat it for more); the rest keep their defaults. `--max-position`,
`--max-order-quantity` and `--max-open-orders` override the strategy's limits. A backtest's day is one session,
09:30-16:00 ET.

Restart `chronos-serve` and the strategy is in the Strategy Lab's list, its form built from its params; a run of it can
be compared, verified and watched in the Trading Terminal like any other.

### 5. Test it

The registry test proves a plugin is deterministic and leaves history alone; it cannot know what the strategy should
do. For that, move the class into a header under `include/simulator/strategies/` and test it on events you build,
as [`tests/strategies/orb_test.cpp`](../tests/strategies/orb_test.cpp) does for the ORB, then add the test file to
`simulator_tests` in `CMakeLists.txt`.

## A strategy written in the browser

### The Strategies page

Open `chronos-serve` and pick **Strategies** in the top bar (`#strategies`). The page has three panels: your scripts on
the left, each marked ok or error; the editor; and the guidelines beside it, the rules of
[Before you write one](#before-you-write-one) as they apply to a script, with the functions a script can call.

1. **Name a strategy** (1-40 of `a-z`, `0-9` and `_`) and press **New**. It starts from a template that runs: a quote
   on each side at the touch, with two params and its limits.
2. **Edit, then Save** (or Cmd/Ctrl+S). Save keeps the script even if it does not load, and loads it at once: the line
   under the editor lists its params and limits, or the error and its line (`load: script:12: '=' expected near
   'x' (line 12)`). A script that loads is offered in the Strategy Lab as "<name> (script)".
3. **Test it in the Strategy Lab.** Pick it, set its params in the form, pick a symbol and one day, and Run. The results
   show its fills, its own stats (each named after it) and a **Log** tab with the lines it logged. Run it again with
   other params, compare two runs, Verify one, and Watch it in the Terminal to a reproduced-or-not verdict. A script
   that errors fails the run, and the run says why and where: `strategy_error: on_market: script:7: attempt to index
   a nil value (local 'x')`.

A run keeps the source it ran, so editing or deleting the script later never changes a saved run: Verify and Watch run
the run's own copy. **Edit as new** is a new run: it runs the script as saved now, with the old run's settings. Scripts are files, `strategy-scripts/<name>.lua` in the server's working directory (in Docker, the
state volume), at most 200 of them and 48 KiB each.

### A script

```lua
summary = "Quotes both sides at the touch"  -- shown in the Lab's form

-- param(name, default, {min, max, help}) declares a param and returns its value in this run.
local size = param("size", 100, {min = 1, max = 10000, help = "shares a quote"})

-- The run's risk limits unless the Lab's form sets its own; here they follow a param.
limits{max_position = 500, max_order_quantity = size, max_open_orders = 2}

local bid_id, ask_id, fills = nil, nil, 0

function on_market(e)
  if not trading() then return end
  local bid = best_bid()
  local ask = best_ask()
  if bid and not bid_id and position() + size <= 500 then bid_id = buy(bid, size, "post_only") end
  if ask and not ask_id and position() - size >= -500 then ask_id = sell(ask, size, "post_only") end
end

function on_report(r)
  if r.type == "fill" then
    fills = fills + 1
    stat("Fills", fills)
    log("fill", r.side, r.qty, "at", dollars(r.price))
  end
  if r.type ~= "accepted" and r.leaves == 0 then  -- this order is done: quote that side again
    if r.id == bid_id then bid_id = nil elseif r.id == ask_id then ask_id = nil end
  end
end
```

The top level runs once when the script loads: it declares the params and limits and sets up state. A script defines
at least one of the three callbacks.

### Its API

| Call | Gives or does |
|---|---|
| `param(name, default, opts)` | declares a param and returns its value in this run; an integer default makes an integer param. `opts.min` defaults to 0, `opts.max` to 10^9 for an integer and no cap for a number; `opts.help` is shown in the form. Top level only; at most 32 |
| `limits{max_position = , max_order_quantity = , max_open_orders = }` | the run's default risk limits; any left out stay at 1,000, 1,000 and 10. Top level only |
| `summary = "..."` | a line shown with the strategy in the Lab |
| `on_market(e)` | after each event the book applied. `e.type` (`add`, `modify`, `replace`, `cancel`, `delete`, `execute`, `clear`, `trade`, `status`), `e.side` (`buy`, `sell`), `e.price`, `e.qty`, `e.time`, `e.order_id`, and `e.trade_price`, `e.trade_qty` when the event printed a trade |
| `on_report(r)` | a report on one of your orders. `r.type` (`accepted`, `rejected`, `fill`, `cancelled`, `cancel_rejected`, `expired`), `r.id`, `r.side`, `r.price` (a fill's), `r.qty`, `r.leaves`, `r.liquidity` (`maker`, `taker`, `none`), `r.reason` (`would_cross`, `invalid_order`, `not_trading`, `off_tick`, `none`), `r.time` |
| `on_timer(id)` | a timer you set has come due |
| `best_bid()`, `best_ask()` | the best level's price, shares and orders, or nil |
| `level(side, n)` | the nth best level (1 to 64) of `"buy"` or `"sell"`: price, shares and orders, or nil |
| `buy(price, qty, tif)`, `sell(price, qty, tif)` | sends an order: `tif` is `"ioc"` (the default) or `"post_only"`. Returns its id, or nil and why: `order_too_large`, `position_limit`, `too_many_open_orders` or `closed` (outside the trading window). A price or quantity that is not a positive integer is an error |
| `cancel(id)` | cancels a resting order. With orders, at most 256 a call: each makes a report, which a session must log |
| `position()`, `cash()`, `pnl()`, `open_orders()` | the portfolio: shares; money in ticks × shares (divide by 10^`price_scale` for dollars) |
| `now()`, `trading()` | the exchange time in ns; whether the symbol is trading (not halted, paused or quotation only) |
| `session()` | the open and close, in ns, of the regular session now is in or before; nil after the last |
| `set_timer(at, id)` | `on_timer(id)` at exchange time `at`, which must be after now (a timer at now could fire forever); `id` from 0 to 2^62. False if refused, or if 64 are already pending |
| `stat(name, value, is_price)` | a counter shown after the run; with `is_price` a price in ticks shown in dollars. At most 32 |
| `log(...)` (or `print`) | a line in the run's Log tab, its values separated by spaces; the first 200 lines, each cut at 300 characters |
| `ticks(dollars)`, `dollars(ticks)`, `price_scale` | prices: `ticks(9.99)` is 99,900 at ITCH's scale of 4 |

Every one of these but `param` and `limits` works only inside a callback. A script also has the base functions but
`dofile`, `loadfile`, `load`, `require`, `collectgarbage` and `xpcall`, and Lua's `string`, `table`, `math` and `utf8`
libraries, with these changes, each so that no work escapes the budget and no address reaches the script:

- `string.find` matches plain text only, a needle of at most 256 bytes; `string.match`, `gmatch` and `gsub` are gone,
  as Lua's patterns can backtrack for ever inside one call;
- `string.rep` makes at most 2^24 copies; `table.move` is gone; `table.sort`, `table.insert` and `table.remove` work
  as before but count their comparisons and shifts;
- `setmetatable` refuses a metatable with `__gc`, as Lua runs finalizers outside the budget;
- `tostring`, `log` and `string.format`'s `%s` show a table or a function as its type, and `%p` is refused.

### The sandbox, and why runs repeat

- **No way out.** There is no `io`, `os`, `package` or `debug`, so a script reads no file, clock or environment and
  starts no process. A script is text: compiled bytecode is refused.
- **The same run every time.** The engine is deterministic, and so is the script's Lua: string hashes start from a
  fixed seed (Lua's own would come from the clock), so `pairs` visits string and number keys in the same order every
  run, `math.random` starts from the same seed, and `table.sort` picks its pivots the same way every run, so elements
  that compare equal end in the same order; `math.randomseed(n)` takes only a seed of the script's own. No address
  is shown. What still differs from one run to the next is the order `pairs` visits tables or functions used as keys:
  do not let a decision depend on it.
- **Bounded.** Each call into the script (its top level, or one callback) may run 10 million Lua instructions and
  allocate 1 GiB, and the script may hold 64 MiB; past any of them it stops with an error naming the line. Those are
  counted, not timed, so a script stops at the same place every run. Once a call is over its budget, every
  instruction raises the error again, so a `pcall` cannot catch it and go on. A call that still runs on, in work
  neither count sees (comparing very long strings, say), stops at 30 s; that one is timed, and the run fails.
- **Contained.** An error in a callback stops the strategy and fails the run with the reason. In the terminal a strategy
  that errors stops trading and the replay goes on, but the terminal does not show why: run it in the Lab to see.
- **Reused tables.** `e` and `r` are the same table every call, refilled: copy a field to keep it.

### Its HTTP API

The page uses these, and so can a script of your own (send `Content-Type: application/json` and the page's `Origin`,
as every change to the Lab must):

| Request | Does |
|---|---|
| `GET /api/scripts` | each saved script's name, whether it loads, and why not |
| `GET /api/scripts/<name>` | its source, and what loading it finds: `{ok, summary, params, limits}` or `{ok: false, error}` |
| `PUT /api/scripts/<name>` | saves `{"source": text}` (48 KiB at most) and answers as `GET` does; 409 past 200 scripts |
| `DELETE /api/scripts/<name>` | deletes it; saved runs keep their copies |
| `POST /api/backtests` | with `"strategy": "script:<name>"`: a run of the saved script, its `params` checked against its own |

A run of a script can also carry its own `"source"` instead of naming a saved one, which is how a saved run's file holds
it. Over the WebSocket, `attach` takes a script only with its `source`.

## How it works

[`strategy_registry.hpp`](../include/simulator/engine/strategy_registry.hpp) holds the registry: a list of
`StrategyInfo` (name, label, summary, params, and functions for its limits, its check and building it), sorted by
name. `register_plugin` builds one from a typed `StrategyPlugin` and wraps the strategy in a `StrategyModel`, an
`AnyStrategy` whose three callbacks are virtual. Every plugin is compiled into an object library linked into
`simulator`, `chronos-serve` and the tests, so each file's registration runs at start-up although nothing calls it.

Front ends see only the registry: the CLI parses `--param` against a strategy's params, the server's
`/api/strategies` lists them for the Lab's form, a run's spec and the command log name a strategy and carry its
params, and the Lab shows its stats. A param list is sorted by name and holds every param, so a saved run states every
value it ran with.

The virtual call costs nothing measurable: on SPY's full day (2.1 million events, one thread, 11 alternating runs on
an Apple M4) the market maker's median went from 109.8 to 108.4 ns an event and the imbalance taker's from 93.1 to
95.0, within the runs' spread, with every fill, PnL and book unchanged.
