# Chronos

[![CI](https://github.com/JaskaranSingh-01/Chronos/actions/workflows/ci.yml/badge.svg)](https://github.com/JaskaranSingh-01/Chronos/actions/workflows/ci.yml)

Chronos is a deterministic C++20 market simulator for NASDAQ TotalView-ITCH 5.0. It rebuilds the full L3 order book of
every symbol of a trading day in one pass, backtests strategies against that book with exact queue position, and serves
an interactive Trading Terminal and Strategy Lab in the browser. Simulated orders never change the replayed history,
and the same input always gives the same books, fills and PnL, byte for byte.

![The Trading Terminal replaying a market maker's backtest on AAPL](docs/images/terminal.png)

## The evidence

NASDAQ's public sample day, 2019-12-30: 8.25 GB, 268,744,780 messages, 8,906 symbols.

| | |
|---|---|
| **Throughput** | **4,885,679 events/s**: the full day, every symbol, 264,469,515 events in 54 s on an Apple M4, a decoder thread beside the book thread. That is 22 % more than the previous engine, measured interleaved beside it, for 65 % more CPU; on one thread the engine does 3,385,625, 15 % below it (the profile says why) |
| **Correctness** | 0 rejected events; every book empty after the last message; 258,271 periodic invariant checks, each of the book the event changed, then of all 8,906 books at the end |
| **Determinism** | the same counts and combined digest, `6897f945bcef75c2`, from list and ladder books, one thread and two, and the previous engine (with every book empty at the close, it fingerprints each symbol's adds and replaces); on a 256 MiB slice, whose books are still live at its end, the same book digests from the Linux/g++ Docker image as from the macOS/clang build |
| **Allocations** | 0 in the replay, counted by a replacement `operator new`, with the heap unchanged; CI checks the hot path |
| **Per-event cost** | 204.7 ns mean; 191 / 447 / 863 ns at p50 / p99 / p99.9 of 1,024-event batch means (the book thread; the clock cannot time one event: see [performance](docs/performance.md#method)) |
| **Tests** | 723 C++ tests, 126 Vitest, 14 Playwright end-to-end; CI on Linux and macOS with ASan, UBSan and TSan |

[docs/performance.md](docs/performance.md) has the method and every configuration's numbers, and
[bench/RESULTS.md](bench/RESULTS.md) the record of every run.

## What it does

- **Decodes ITCH 5.0** into one 48-byte event type that every source shares, reading only the selected symbols'
  messages. A per-symbol store (zstd segments, hash-checked) lets one symbol's day load in under a second.
- **Rebuilds the L3 book**: every order, in FIFO order, at every price. Price levels live in preallocated pools behind
  open-addressing indices, as a list or as a ladder of slots a cent apart around the best; nothing allocates during a
  replay.
- **Decodes and applies on two threads**, through a lock-free single-producer single-consumer ring, with the same books
  as one thread.
- **Backtests** through one deterministic event loop: constant order and report latencies, a fill model that tracks each
  simulated order's queue position among the historical orders and fills it by four explicit rules with a trade-price
  guard, a portfolio with pre-trade risk limits and maker/taker fees.
- **Takes strategies as plugins**: a strategy is one file in [`strategies/`](strategies/) that registers itself, and the
  CLI, the server and the Lab's form pick it up with no other change ([docs/strategies.md](docs/strategies.md)). Four
  ship: a market maker, an imbalance taker, an opening range breakout and the guide's example.
- **Or writes them in the browser**: the Strategies page edits Lua scripts that run in a sandbox (no clock or files,
  bounded instructions and memory, fixed seeds), checks each on save, and offers it in the Lab. A run keeps the source
  it ran, so it verifies and replays in the terminal to the same digests; a Lua port of the imbalance taker makes the
  C++ plugin's exact fills on SPY's full day.
- **Serves the Trading Terminal**: replay any symbol at 1× to Max, step, seek, across several days; a TradingView-style
  chart, the L3 book with your queue position, and an order ticket that trades against the historical queue.
- **Serves the Strategy Lab**: run backtests from the browser, compare runs, verify a run against its recorded digests,
  and watch it replay in the terminal to a reproduced-or-not verdict.

![The Strategy Lab comparing an opening range breakout with a market maker on AAPL](docs/images/strategy-lab.png)

## Quick start

**Docker** (the UI, the server and the CLI in one image):

```bash
docker compose up          # then open http://localhost:8080/ and enter the token: chronos
```

With no data it serves two generated sample days; put a real day in `data/itch` (below) and run it again.

**From source** (CMake 3.20+, a C++20 compiler, POSIX; Node 24 for the UI):

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
(cd web && npm ci && npm run build)

mkdir -p data/itch
curl -fL 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz' | gunzip > data/itch/12302019.NASDAQ_ITCH50.bin

./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30        # every book, the whole day
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy market_maker
./build/chronos-serve --data data/itch                                                         # http://127.0.0.1:8080/
```

[docs/getting-started.md](docs/getting-started.md) walks through each step.

## How it fits together

```text
ITCH 5.0 day ──► ItchSource ──► SPSC ring ──► MarketReplay::run ──► MarketBooks (an L3 book per symbol)
   │ (mmap)       decode, filter     (decoder thread → book thread)
   │
   └─► per-symbol store ──► one symbol's segment ──► EventLoop<Strategy>
                                                      ① ExecutionSimulator: our shadow orders, on the pre-event book
                                                      ② MarketBook::apply: the only writer of the history
                                                      ③ Portfolio::mark
                                                      ④ Strategy::on_market, on the post-event book
                                                             │
                              chronos-serve: sessions, command log, Lab worker ──► WebSocket ──► React UI
```

[docs/architecture/](docs/architecture/README.md) is the design: the components and threads, the event and execution
model, the invariants, the decisions, and what is still open. [docs/code-map.md](docs/code-map.md) maps it to the code
and lists the eight files to read first.

## Design decisions

- **History and simulation never mix.** The historical book has one mutator, and our orders are shadow orders that
  never enter it. A backtest therefore leaves the same book digest as a plain replay, which the tests assert.
- **One loop, no wall clock.** Market events and our own events merge in (timestamp, stream, sequence) order, and a tie
  goes to the market, so an order never jumps ahead of history stamped at the same nanosecond.
- **A conservative fill model.** A resting order joins the back of the visible queue at its price and fills only by
  rules a real queue would: shares ahead executed or cancelled, a trade behind or through it, a crossing order. Prices
  must be whole cents at $1 or more, as Reg NMS Rule 612 requires, so no order jumps a queue by a fraction of a cent.
  A FIFO-anomaly counter checks the model against real days, and once found a real bug in it.
- **Allocation-free by construction.** Capacity is a run parameter: pools and indices are reserved up front, and a full
  one stops the run cleanly instead of growing mid-replay.
- **Measured, not assumed.** Optimisations follow the profile, and the defaults follow a benchmark run interleaved with
  the previous engine on the same machine: one candidate was rejected, and the ladder's slots were widened from price
  units to cents once review showed its $0.0256 window held about one level a side.
- **Reproducible from the browser.** Every session's commands are logged with the build and each input segment's hash,
  and a Lab run can be verified or watched again to the same digests, every fill compared in order, or is reported as
  run on different code or changed input.

## Documentation

| | |
|---|---|
| [Getting started](docs/getting-started.md) | Docker, the native build, the data, first runs |
| [Architecture](docs/architecture/README.md) | the design: purpose and roadmap, components, events and execution, the server, performance and determinism, decisions, acceptance criteria, invariants, open questions |
| [Code map](docs/code-map.md) | each component in the code, where each invariant is enforced, the layout, where to start reading |
| [Market data](docs/market-data.md) | the event contract, ITCH 5.0, the per-symbol store |
| [Order book](docs/order-book.md) | the L3 book, allocation-free storage, the list and the ladder, invariants, digests |
| [Backtesting](docs/backtesting.md) | the event loop, the fill model, risk and fees, the strategies |
| [Writing a strategy](docs/strategies.md) | as a C++ plugin or a Lua script in the browser: the rules, the template, the API, the sandbox, testing |
| [Performance](docs/performance.md) | the numbers, the profile, the method |
| [Server](docs/server.md) | sessions, the command log, the Strategy Lab, the HTTP and WebSocket protocol, security |
| [UI](docs/ui.md) | the Trading Terminal, the Strategy Lab and the Strategies page, and how the page stays at 60 frames a second |
| [CLI](docs/cli.md) | every option, exit codes, `--json` |
| [Testing](docs/testing.md) | the suites, how correctness is checked, sanitizers, CI |
| [Limitations and roadmap](docs/limitations.md) | what the model assumes, and what comes next |

## Status

The engine, the backtester, the per-symbol store, the server and both UI pages are complete and tested; CI runs on
every push. The model's assumptions (no market impact, constant latencies, toy strategies, per-share fees only) and the
roadmap (faster level placement, regulatory sell fees, a LOBSTER adapter) are in
[docs/limitations.md](docs/limitations.md).

Market data is never committed or redistributed here: `data/` is in `.gitignore`.
