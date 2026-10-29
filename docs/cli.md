# Command-line reference

Two binaries: `simulator`, the engine's CLI, and `chronos-serve`, the server behind the Trading Terminal, the
Strategy Lab and the Strategies page ([server.md](server.md)).

## `simulator`

`./build/simulator --help` prints:

```text
usage: simulator replay <itch-file> (--symbol AAPL | --symbols AAPL,MSFT | --all)
                        --date 2019-12-30 [options]
       simulator replay --store DIR --symbol AAPL --date 2019-12-30 [options]
       simulator backtest (<itch-file> | --store DIR) --symbol AAPL --date 2019-12-30 [options]
                          [backtest options]
       simulator -h | --help | --version

options, both commands:
  --symbol AAPL           one symbol, 1-8 characters (backtest takes only --symbol)
  --date YYYY-MM-DD       the session date (required)
  --store DIR             read the --symbol's day from the per-symbol store chronos-serve builds
                          (its --store, by default <data>/store) in place of <itch-file>
  --utc-offset -05:00     offset from UTC (+HH:MM or -HH:MM); default US Eastern for --date
  --allow-truncated-tail  a file cut mid-message ends there instead of failing
  --depth N               book levels to print per side, at most 1000 (default 5)
  --check-invariants N    check the book every N applied events (default 0 = only at the end)
  --capacity N            order slots reserved for the book, at most 1073741824 (default 1048576);
                          with --symbols or --all, shared by every book (default 4194304)
  --book B                the price levels: ladder (default), a window of 256 cents around each
                          side's best over its list and index; or list, the list and index alone
  --threads N             2 (default): one thread decodes, one applies; 1: one thread does both
  --json PATH             also write the results, metrics, build and input as JSON;
                          backtest adds its config, fills, fees and PnL

replay only:
  --symbols A,B,C         several symbols in one pass, each into its own book
  --all                   every symbol in the file
  --latency               time every batch of 1024 events; print ns/event percentiles
                          over the batches
  --latency-batch N       events per timed batch (implies --latency)

backtest runs a strategy against the replayed book, which our orders never change:
  --strategy NAME         a registered strategy (default imbalance): the list, with each one's
                          params, follows this help
  --param NAME=VALUE      set one of its params; repeat for more; the rest keep their defaults
  --order-latency-ns N    strategy to exchange, in nanoseconds (default 50000)
  --order-latency-us N    the same in microseconds
  --report-latency-ns N   exchange back to strategy and portfolio (default 50000)
  --report-latency-us N   the same in microseconds
risk limits (default: the strategy's own, from its params):
  --max-position N        |position| limit in shares
  --max-order-quantity N  the largest order in shares
  --max-open-orders N     live orders at once
fees, per share in price ticks ($0.0001 for ITCH):
  --taker-fee-ticks N     paid when taking liquidity (default 0)
  --maker-fee-ticks N     paid when providing it; negative = rebate (default 0)
a backtest trades the whole file; its strategy is told one session, 09:30-16:00 ET

  -h, --help              print this help
  --version               print the commit, build type and compiler

exit status:
  0  the run succeeded and every check passed (also --help and --version)
  1  the run failed: a source error (a file cut mid-message without --allow-truncated-tail,
     no stock directory message for --symbol, ...), an out-of-order timestamp, a failed
     book invariant (e.g. a book crossed in regular trading hours while it is trading), or
     a book out of room (raise --capacity); rejected events alone are counted, not failures
  2  bad usage, an input file that cannot be opened, a symbol the store does not hold on --date,
     a store that cannot be read, or a --json file that cannot be written

strategies (--strategy NAME, then --param NAME=VALUE for any param):
  imbalance: A toy: an IOC at the touch when one top-of-book size is threshold times the other
    threshold              3 (1 to any): one side's top size over the other's that triggers an order
    clip                   100 (1 to 1000000000): shares per order
    max_position           500 (0 to 1000000000): |position| the strategy itself will not exceed
    cooldown_ns            100000000 (0 to 1000000000000000): the wait after each order, in ns
  market_maker: A toy: post-only quotes joining the best bid and ask, one per side
    quote_size             100 (1 to 1000000000): shares per quote
    max_position           500 (0 to 1000000000): |position| a fill of the next quote may not exceed
    requote_cooldown_ns    100000000 (0 to 1000000000000000): the youngest quote that may be moved, in ns
    improve_ticks          0 (0 to 1000000000): grid ticks inside the touch, while the spread is over twice it
  orb: Opening range breakout: an IOC on the first trade beyond the day's opening range; a stop, and a flatten 5 minutes before the close
    range_minutes          15 (1 to 390): the opening range from the open, minutes
    clip                   100 (1 to 1000000000): shares per entry
  trend_taker: The plugin guide's example: an IOC buy after a run of rising trades, sold after a hold
    clip                   100 (1 to 1000000): shares per entry
    run_length             3 (1 to 1000): rising trades in a row that trigger an entry
    hold_ns                1000000000 (1 to 3600000000000): how long a position is held, in ns
```

A bad argument prints a two-line error on stderr and exits 2.

### Examples

```bash
# one symbol from the slice
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin --symbol AAPL --date 2019-12-30 --allow-truncated-tail
# several symbols in one pass, each into its own book
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin --symbols AAPL,MSFT --date 2019-12-30 --allow-truncated-tail
# every symbol of the full day, with per-batch latency percentiles
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30 --latency
# one symbol from the per-symbol store chronos-serve builds
./build/simulator replay --store data/itch/store --symbol AAPL --date 2019-12-30
# backtests
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy market_maker --maker-fee-ticks -20
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy imbalance --taker-fee-ticks 30
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy orb --param range_minutes=30
# a param and a risk limit: --param sets the strategy's own, --max-position the gateway's
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy imbalance --param clip=50 --max-position 200
```

### A replay's report

On the slice, for AAPL (exit status 0):

```text
Source
  messages 7844756: 6530347 other symbols, 1305990 unused types, 8418 events; locate 13
  truncated tail   yes (last partial message ignored)

Events 8418, 03:09:14.653737350 to 09:30:10.715766934 local
  Add              4672
  Modify           0
  Replace          58
  Cancel           10
  Delete           2998
  Execute          494
  Clear            0
  Trade            185
  Status           1

Applied 8418, rejected 0

Book, top 5 levels per side
  side          price         size   orders
  ask        289.4900          100        1
  ask        289.4700            9        1
  ask        289.4300          100        1
  ask        289.4100          300        1
  ask        289.3800          587        4
  bid        289.3200           65        1
  bid        289.2900          135        3
  bid        289.2800            2        1
  bid        289.2700          304        2
  bid        289.2500          741        6
  spread 0.0600, orders 1391, peak orders 1391, crossed no, locked no, trading state Trading

Invariants at end: ok (0 periodic checks passed)
Digest            7f1e922654415cf3
```

The run also prints its configuration first, and `Elapsed`, `Memory` and `Ladder windows` lines last. With `--all`,
it lists the 10 symbols with the most events, then up to 40 others with rejections or orders left and a count of the
rest, then the combined digest.

### `--json`

`--json PATH` writes one JSON object with `"schema": 1` and the members `date`, `build` (commit, dirty flag, build type,
compiler), `machine`, `input` (the file, its size and a fingerprint), `run` (the symbols, capacity, book, threads and
invariant-check interval), `result`, `time`, `memory` and `latency`; `backtest` adds a `backtest` member with its
config, fills, fees, PnL, the strategy's `params` and its own `stats` (a price stat in ticks). `bench/summary.py` reads
these files.

## `chronos-serve`

`./build/chronos-serve --help` prints:

```text
usage: chronos-serve --data DIR [--date YYYY-MM-DD] [--host 127.0.0.1] [--port 8080] [--web web/dist]
                     [--token T] [--fsync] [--allow-truncated-tail] [--store DIR] [--book list|ladder]

Serves an interactive replay of the days in DIR, the files named MMDDYYYY.NASDAQ_ITCH50.bin; the UI picks the
symbol and its days. A new file is read once, about a minute a day, into the per-symbol store, which every
replay of a symbol reads, and its hash kept in runs/fingerprints.json. Strategy Lab runs are kept in
runs/<id>.json and the strategies written on the Strategies page in strategy-scripts/<name>.lua, both in the
working directory.
  --date YYYY-MM-DD       the starting day (default the latest)
  --store DIR             the per-symbol store (default DIR/store), about a third the size of its days;
                          one chronos-serve at a time uses it
  --host IP               the address to bind, an IP address (default 127.0.0.1); any other requires a
                          token, and one is made and printed once if --token is not given; it guards
                          trading, not availability: put a buffering reverse proxy in front
  --port N                default 8080
  --web DIR               the built UI (default web/dist)
  --token T               /api needs 'Authorization: Bearer T'; a WebSocket's first message is auth{T}
  --fsync                 sync each command line of the log (sessions/<id>.jsonl) to the device
  --allow-truncated-tail  a file cut mid-message ends there instead of failing
  --book B                every book's price levels: ladder (default) or list, as simulator's --book

exit status: 0 after SIGINT or SIGTERM; 2 bad usage, a store in use or that cannot be locked, no day ready in
DIR or none for --date, a starting day with no symbol's market events, a log that cannot be written, or an
address that cannot be bound
```
