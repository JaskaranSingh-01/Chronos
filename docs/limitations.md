# Limitations, assumptions and the roadmap

## What the simulation assumes

- **No market impact.** Our fills never remove historical liquidity, so repeated IOCs can take the same visible shares
  again. Fill rates for aggressive orders are optimistic.
- **Order types:** IOC, post-only Day orders, and in the terminal market orders (an IOC within 1 % of the best opposite
  price). A marketable Day order is rejected, not partly filled.
- **The fill rules are (a), (b), (c) and (d) with the trade-price guard** ([backtesting.md](backtesting.md)). Hidden and
  non-displayed liquidity is invisible: ITCH `P` prints never fill our orders, and neither do cross trades (`Q`).
  Opening and closing crosses and trading halts are not modelled in fills.
- **Constant latencies.** The order latency also stands for the market-data delay. There are no latency
  distributions or queueing at the exchange.
- **Fees are per share only:** maker and taker fees or rebates. SEC Section 31 and the FINRA TAF on sells are not
  modelled yet.
- **One symbol per backtest.** `replay` takes one symbol, a list or all of them; `backtest` and the Lab take one, over
  one or more days.
- **The strategies are toys,** written to exercise the fill model and the loop, not to make money.

## What the engine assumes

- **L3 data only.** Every source emits per-order events. The CLI and the server read NASDAQ TotalView-ITCH 5.0;
  `CsvSource` is available to the library and tests.
- **Session times.** The CLI's regular trading hours are fixed at 09:30 to 16:00 ET, so half-day sessions are not
  handled there (the server reads each day's own System Events). The default US Eastern offset follows the
  daylight-saving rules in force since 2007; pass `--utc-offset` for earlier dates.
- **Capacity is fixed per run.** Pools and indices are sized before the replay from `--capacity`; a full one stops the
  run as `CapacityExhausted` instead of growing.
- **The digest** is a regression check, not a proof: with every book empty at the end of a day it fingerprints each
  symbol's count of Adds and Replaces. The invariants and the 0 rejections carry correctness.
- **Platforms.** POSIX only (`mmap`). Performance is measured on macOS on an Apple M4; CI builds and tests on Ubuntu and
  macOS, and the Docker image runs on Linux. AddressSanitizer and ThreadSanitizer run only on Linux, as both hang at
  start-up on macOS 27.

## What the server assumes

- **One session at a time,** with one controlling tab; the Lab runs one backtest at a time with 8 waiting.
- **A local tool.** The token guards trading, not availability; put a buffering reverse proxy in front of anything
  public.
- The Lab keeps up to 50,000 fills of each run in memory and on disk; a few hundred saved runs would call for loading
  results on demand.

## Roadmap

- **Faster level placement:** placing new price levels is still 40 % of the book thread's time; a bitmap of the ladder's
  slots would make its scan a few words, then cheaper ITCH decoding.
- **Regulatory sell fees:** SEC Section 31 and FINRA TAF.
- **LOBSTER:** a second real adapter, and a check of `MarketBook` against LOBSTER's own book after every message.
- **`MergedSource`:** a k-way merge by timestamp, for multi-venue replay.
- **Replay through `EventLoop`:** run the replay through `EventLoop` with a null strategy once its throughput is shown
  to match `MarketReplay::run`.
- **The UI:** an engine monitor and a symbol browser.
- **CSV input in the CLI,** an open question.
