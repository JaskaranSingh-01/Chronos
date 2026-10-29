# Limitations, assumptions and the roadmap

## What the simulation assumes

- **No market impact.** Our fills never remove historical liquidity, so repeated IOCs can take the same visible shares
  again. Fill rates for aggressive orders are optimistic.
- **Order types:** IOC, post-only Day orders, and in the terminal market orders (an IOC within 1 % of the best opposite
  price, the band rounded down to the price grid). A marketable Day order is rejected, not partly filled, and so is a
  price off the cent grid at $1 or more (`OffTick`).
- **The fill rules are (a), (b), (c) and (d) with the trade-price guard** ([backtesting.md](backtesting.md)). Hidden and
  non-displayed liquidity is invisible: ITCH `P` prints never fill our orders, and neither do cross trades (`Q`). The
  opening and closing crosses are not modelled as auctions: executions in their seconds fill resting orders by the same
  rules, at our limit price rather than the cross price. An order that arrives while the book is not trading (halted,
  paused or quotation only) is rejected, and resting orders do not fill until it trades again.
- **Constant latencies.** The order latency also stands for the market-data delay. There are no latency
  distributions or queueing at the exchange.
- **Fees are per share only:** maker and taker fees or rebates. SEC Section 31 and the FINRA TAF on sells are not
  modelled yet.
- **One symbol per backtest.** `replay` takes one symbol, a list or all of them; `backtest` takes one symbol on one day,
  and the Lab one symbol over up to 32 days.
- **The strategies are toys,** written to exercise the fill model and the loop, not to make money.
- **Strategies written in the browser** run only in the server (the Lab and Watch), not the CLI; cost about 230 ns an
  event more than a C++ plugin; and may not use the clock, files, Lua's patterns, finalizers, or more than 10 million
  instructions, 1 GiB allocated or 30 s a call, or 64 MiB held. In the terminal, a script that errors stops trading
  without saying why. A plugin needs a rebuild and a server restart.

## What the engine assumes

- **L3 data only.** Every source emits per-order events. The CLI and the server read NASDAQ TotalView-ITCH 5.0;
  `CsvSource` is available to the library and tests.
- **Session times.** Some trading hours are fixed at 09:30 to 16:00 ET: the CLI's session, the crossed-book check and
  the fill attribution's phases (the Lab's included), so half days are wrong there. The server reads each day's own
  session from its System Events for a strategy's trading window and the sessions it is built with. The CLI never
  closes the gateway: a CLI backtest trades the whole day file. The default US Eastern offset follows the
  daylight-saving rules in force since 2007; pass `--utc-offset` for earlier dates.
- **Capacity is fixed per run.** Pools and indices are sized before the replay from `--capacity`; a full one stops the
  run as `CapacityExhausted` instead of growing.
- **The digest** is a regression check, not a proof. A book's digest hashes the orders still resting in it, so after
  a full day, when the book is empty, `replay --symbol` and `backtest` print the same constant, `cbf29ce484222325`, for
  every symbol. Only the combined digest of `--symbols` and `--all`, which also mixes in each book's last sequence
  number, fingerprints each symbol's count of Adds and Replaces; a backtest's execution and portfolio digests
  fingerprint its fills and account. The invariants and the 0 rejections carry correctness.
- **Platforms.** POSIX only (`mmap`). Performance is measured on macOS on an Apple M4; CI builds and tests on Ubuntu and
  macOS, and the Docker image runs on Linux. AddressSanitizer and ThreadSanitizer run only on Linux, as both hang at
  start-up on macOS 27.

## What the server assumes

- **One session at a time,** with one controlling tab; the Lab runs one backtest at a time with 8 waiting.
- **A local tool.** The token guards trading, not availability; put a buffering reverse proxy in front of anything
  public. A client that stops reading its WebSocket has its snapshots buffered in the server, about 1 MB a second,
  until it reads or disconnects.
- **The command log is written behind the engine.** A command can apply before its line is on disk; the client is
  acknowledged once the line is written. A failed write (a full disk) or a full queue fails the log: the lines it could
  not take or write are lost, their commands are answered `log_unavailable` though they may have applied, and the
  session refuses commands after it.
- **The terminal's candles and tape** count every execution message, non-printable `C` executions included, and leave
  out the cross's own print (`Q`), so the auction minutes show less volume than the tape a vendor would print.
- **The terminal's fills and PnL curve are what the page saw.** A reload or a second tab gets the account and the
  open orders from the server's state, but the Fills list and the curve start empty: the server would have to replay
  the generation's fills, which pass through its report ring only once.
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
