# Writing a strategy

A strategy is a plugin: one C++ file in [`strategies/`](../strategies/) that holds the strategy, its params and a
registration. The next build picks the file up, and the CLI, the server and the Strategy Lab's form offer it with no
change anywhere else. Four ship, and each is a worked example:

| File | `--strategy` | Shows |
|---|---|---|
| [`trend_taker.cpp`](../strategies/trend_taker.cpp) | `trend_taker` | the whole strategy in its plugin file: the template to copy |
| [`imbalance.cpp`](../strategies/imbalance.cpp) | `imbalance` | IOCs; a real (floating-point) param with no upper cap |
| [`market_maker.cpp`](../strategies/market_maker.cpp) | `market_maker` | post-only quotes, cancels and requotes |
| [`orb.cpp`](../strategies/orb.cpp) | `orb` | each day's regular session from `make`, a timer, a price stat, a `check` |

The last three keep the strategy itself in a header in
[`include/simulator/strategies/`](../include/simulator/strategies/), so unit tests can drive it directly.

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
