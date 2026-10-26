# Chronos

Chronos is a deterministic C++20 event-driven market simulator: one thread decodes the feed while another applies it
to the books, and one thread alone gives the same books. It rebuilds the L3 order book of every symbol of a NASDAQ
TotalView-ITCH 5.0 day in one pass, and backtests strategies against that book with exact queue position. Simulated
orders never change the replayed history. At run time it needs only the C++ standard library and POSIX.

## Verified results

**Data:** NASDAQ's public TotalView-ITCH 5.0 sample for 2019-12-30, `12302019.NASDAQ_ITCH50`: 8,251,407,909 bytes
decompressed, 268,744,780 messages. The replay numbers come from the benchmark runs of the default configuration in
[`bench/results/2026-10-24-b6f3517-dirty-interleaved-list-2/`](bench/results/2026-10-24-b6f3517-dirty-interleaved-list-2/summary.md),
measured on 2026-10-24 and 2026-10-26. Every configuration and the previous engine gave the same counts and digest.

### Full day, every symbol in one pass

| | |
|---|---|
| Events | 264,469,515 |
| By type | Add 118,631,456 · Delete 114,360,997 · Replace 21,639,067 · Execute 5,822,741 · Cancel 2,787,676 · Trade 1,218,602 · Status 8,976 |
| Rejected | 0 |
| Books | 8,906, every one empty after the last event (20:00:00.097 ET) |
| Peak live orders | 1,924,078, all books together |
| Invariants | pass for all 8,906 books at the end of the run |
| Combined digest | `6897f945bcef75c2`, identical in all 34 full-day runs of the engine round: four configurations and the previous engine |

From [`full-1.log`](bench/results/2026-10-24-b6f3517-dirty-interleaved-list-2/full-1.log):

```text
Source
  messages 268744780: 0 other symbols, 4266369 unused types, 264469515 events
  truncated tail   no
Events 264469515, 03:09:14.328694637 to 20:00:00.097129306 local
  Add              118631456
  Modify           0
  Replace          21639067
  Cancel           2787676
  Delete           114360997
  Execute          5822741
  Clear            0
  Trade            1218602
  Status           8976

Applied 264469515, rejected 0

Books 8906: 8906 empty at the end, 0 not empty
...
Live orders       peak 1924078, 0 at the end, all books together (capacity 4194304)
Invariants at end: ok, all 8906 books
                   0 periodic checks passed
Digest (combined) 6897f945bcef75c2
```

What this evidence shows, and what it does not:

- **0 rejections.** Every Add found a new order id. Every Execute, Cancel, Delete and Replace named a live order
  and stayed within its remaining size. (A full pool or index stops the run as `CapacityExhausted`; it is no
  rejection.) A decoder or book bug (a skipped `F` or `C` message, a
  wrong field offset, a wrong replace) typically shows up as `UnknownOrder` or `ExceedsRemaining` rejections.
- **Every book empty after the last message.** Every order added during the day was removed by later messages.
- **Invariants** are checked at the end of the run: each side's levels are strictly ordered from the best, the order
  and level indices map every order and level, level totals match their orders, no level is empty, sequence numbers
  rise from head to tail (FIFO), the ladder's windows match its levels, and the pools' and indices' counts match.
  `--check-invariants N` also checks the book of every N-th applied event. Only those periodic checks test "not
  crossed during regular trading hours", because the end-of-run check falls after 16:00. The runs above used the
  end-of-run check only; the engine round's correctness runs (`--check-invariants 1024`) passed 258,271 periodic
  checks in each configuration ([`bench/RESULTS.md`](bench/RESULTS.md)).
- **The combined digest** is FNV-1a over (symbol, last sequence, book digest) for every book. With every book empty
  at the end, it fingerprints each symbol's count of Adds and Replaces. It is a regression check across code
  changes, not a proof of correctness.
- **The 256 MiB slice** (below) replays every symbol with 0 rejections, and `--all` on it passes the invariant
  check: 6 books are crossed after 09:30, all of them halted (ITCH trading state `H`), and a halted book may cross
  until trading resumes.

### Backtest: SPY market maker

The `mm` strategy quotes 100 shares at the best bid and the best ask with post-only orders.

| Run | Maker fills | Fill rate (of quoted shares) | FIFO anomalies | Total PnL |
|---|---|---|---|---|
| Full day, no fees | 12,843 | 19.69 % | 0 | −$2,726.78 |
| 256 MiB slice (pre-market), no fees | 143 (13,693 shares) | 3.65 % | 0 | $4.77 |

The full-day row was recorded on 2026-10-13 at commit `a5ed53b`; its log is not in the repository, so re-run the
command in [Backtest](#backtest) to reproduce it. The slice row is the example output under
[Backtest](#backtest). These are toy strategies: the PnL is not a trading result.

**The FIFO-anomaly counter found a fill-model bug.** It counts fills by rules (a) or (d) (see
[Fill model](#d2-the-fill-model)) while the model still had historical shares ahead of our order. The same full-day
run before the trade-price guard, also recorded on 2026-10-13, had 583 such anomalies among 13,539 maker fills. The
fill attribution table traced all of them to ITCH `C` executions of price-slid orders: displayed at our price, but
ranked and executed one tick better, so the aggressor never reached our price. With the guard, the count is 0.

### Tests

- `simulator_tests`: 481 pass and 6 skip in RelWithDebInfo, Debug and g++ 14 Release. Five of the skips are the
  real-data tests, which need `CHRONOS_ITCH_FILE`; the sixth is the error check of the conformance suite, which does
  not apply to `VectorSource`. `chronos_server_tests`: 153 pass. `alloc_counter_test`: 3 pass. `ctest` runs 663
  tests, including 19 CLI tests of `simulator` and `chronos-serve`.
- 0 compiler warnings with `-Wall -Wextra -Wpedantic` under Apple clang 17 and g++ 14, Release and Debug.
- UBSan (`-DCHRONOS_SANITIZE=undefined`, where any report aborts): its last local run, of 397 tests, passed them all
  in RelWithDebInfo and in Debug; CI runs it, with ASan and TSan, on every push.
- With `CHRONOS_ITCH_FILE` set to the slice, the 5 real-data tests pass.

## Performance

Chronos processes **4,913,837** market events per second: the full NASDAQ day of 2019-12-30, every symbol
(264,469,515 events), on one machine, an Apple M4, with `--book list --threads 2`.

Full day, `--all`, median of 3 runs. Each round ran the four configurations and the previous engine (commit
`b6f3517`) back to back, in rotating order, so every column met the same machine. Every run gave digest
`6897f945bcef75c2` and 0 rejections.

| | baseline `b6f3517` | list × 1 | **list × 2** | ladder × 1 | ladder × 2 |
|---|---|---|---|---|---|
| CPU s (user + sys, both threads) | 56.7 | 68.7 | 95.1 | 71.6 | 110.9 |
| wall s | 65.7 | 76.9 | 53.8 | 79.8 | 61.9 |
| events/s | 4,026,805 | 3,440,288 | **4,913,837** | 3,314,434 | 4,270,030 |
| ns/event, mean | 248.3 | 290.7 | 203.5 | 301.7 | 234.2 |
| ns/event p50 / p99 / p99.9 | 215 / 639 / 3,199 | 255 / 671 / 2,559 | 191 / 431 / 735 | 271 / 671 / 2,815 | 223 / 495 / 799 |
| allocations in `run()` | 43,910,763 | 0 | 0 | 0 | 0 |
| heap change | not measured | 0 | 0 | 0 | 0 |
| peak footprint MB | 593 | 740 | 744 | 758 | 762 |
| reserved MB | – | 925 | 929 | 1,059 | 1,063 |
| results | [baseline](bench/results/2026-10-24-b6f3517-dirty-interleaved-baseline/summary.md) | [list-1](bench/results/2026-10-24-b6f3517-dirty-interleaved-list-1/summary.md) | [list-2](bench/results/2026-10-24-b6f3517-dirty-interleaved-list-2/summary.md) | [ladder-1](bench/results/2026-10-24-b6f3517-dirty-interleaved-ladder-1/summary.md) | [ladder-2](bench/results/2026-10-24-b6f3517-dirty-interleaved-ladder-2/summary.md) |

The default, list × 2, against the baseline: events/s +22.0 %. On one thread the round is slower than the baseline
(list −14.6 %, ladder −17.7 %); the profile below shows where the book thread's time goes.

- **Machine:** Apple M4 (4 performance + 6 efficiency cores), 16 GiB, macOS 27.0.1, Apple clang 17.0.0, CMake
  `Release` (`-O3 -DNDEBUG`), one or two threads.
- **Interleaved runs.** A one-thread replay that lands on an efficiency core runs about half as fast, so runs made
  one configuration after another on a busy machine are not comparable: this round's first, sequential matrix
  measured one thread at about 1.48 M events/s, and the baseline measured alone that morning gave 2.00 M. Each run
  above started once the load average was below 4.5; round 3 was re-run whole on 2026-10-26, after the Mac slept
  through one of its runs.
- **Per-event figures are a batch-derived per-event estimate.** Each sample is the mean ns/event of a batch of 1,024
  events, as `steady_clock` ticks every 41.67 ns on this machine and cannot time a single event. They show how the
  cost of an event varies across the day, not the latency of any one event. With two threads they are the book
  thread's.
- **Zero allocations in the replay.** The allocation counter counts every `operator new` in the process from just
  before `MarketReplay::run` to just after it, both threads included: 0 in every configuration on the full day. The
  heap's blocks and bytes in use are equal before and after. `scripts/check-hot-path.sh`, run in CI, finds no
  allocator call and no growing container call on the replay path outside its setup.
- **I/O.** The 8.25 GB file does not stay in the page cache of a 16 GiB machine, so every full-day run read it from
  disk again (about 500,000 major faults). Wall time includes that wait.
- **Memory.** Peak RSS counts the resident pages of the mapped input, so the table shows the peak footprint, the
  process's own memory. The reserved bytes are set up before the replay: the order pool, the order and level
  indices, the level pool, and the ladder's windows or the ring.
- **Profile** (`/usr/bin/sample`, 1 ms, list × 2), self time of the book thread inside `MarketReplay::run`:
  `new_level` 43.9 % (placing a new price level, a walk of the list from the best), `remove` 15.1 %, `find` 8.9 %,
  `MarketBook::apply` 8.5 %.

[`bench/RESULTS.md`](bench/RESULTS.md) has the protocol, the min-max ranges, the slice runs, the Google Benchmark
micro-benchmarks, the profiles, the decisions (including a rejected candidate) and the caveats. Reproduce with
`bench/run.sh` (below).

## Quick start

**Requirements:** CMake 3.20 or newer, a C++20 compiler and a POSIX system. Developed on macOS arm64 with Apple
clang 17. GoogleTest v1.16.0, and Google Benchmark v1.9.1 with `-DCHRONOS_BENCH=ON`, come in through FetchContent,
so the first configure needs network access.

### Build and test

```bash
cmake -S . -B build                  # RelWithDebInfo unless CMAKE_BUILD_TYPE is set
cmake --build build -j
ctest --test-dir build               # simulator_tests, alloc_counter_test and the CLI exit-code tests
```

A Debug build:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
ctest --test-dir build/debug
```

**Sanitizers.** `CHRONOS_SANITIZE` is `OFF` (the default), `ON` (= `address,undefined`) or any `-fsanitize=` list.
Every sanitized build adds `-fno-sanitize-recover=all`, so any report aborts and a passing test is a clean one.

```bash
cmake -S . -B build/ubsan -DCHRONOS_SANITIZE=undefined
cmake --build build/ubsan -j
ctest --test-dir build/ubsan
```

**ASan note.** On macOS 27 with Apple clang 17, AddressSanitizer hangs at process startup, so
`-DCHRONOS_SANITIZE=ON` does not work on the development machine and Chronos has not been run under ASan there.
Use `undefined` locally. The GitHub Actions workflow in [`.github/workflows/ci.yml`](.github/workflows/ci.yml)
runs ASan with UBSan under g++ on Ubuntu, and UBSan under clang on Ubuntu and macOS; it has not run yet.

**Formatting.** [`scripts/check-format.sh`](scripts/check-format.sh) `[FILE...]` checks files against
[`.clang-format`](.clang-format) and changes nothing. Use the version CI pins: `pipx install clang-format==20.1.8`.
The existing code has not been reformatted yet.

### Get the data

NASDAQ publishes sample TotalView-ITCH 5.0 days at `https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`, one gzip file per
day named `MMDDYYYY.NASDAQ_ITCH50.gz`. Everything in this README uses 2019-12-30:

```bash
mkdir -p data/itch
curl -fL -o data/itch/12302019.NASDAQ_ITCH50.gz 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz'
gzip -t data/itch/12302019.NASDAQ_ITCH50.gz
gunzip -c data/itch/12302019.NASDAQ_ITCH50.gz > data/itch/12302019.NASDAQ_ITCH50.bin
```

The download is 3,524,013,057 bytes (its size on 2026-10-13), and the `.bin` is 8,251,407,909 bytes. Chronos maps
the uncompressed file; it does not read gzip. `data/` is in `.gitignore`: market data is never committed or
redistributed here.

An optional slice, the first 256 MiB, for quick runs and the real-data tests:

```bash
head -c 268435456 data/itch/12302019.NASDAQ_ITCH50.bin > data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin
```

It covers 04:00 to 09:30:10 ET, so it is pre-market only, and its last message is cut, so pass
`--allow-truncated-tail`.

### Replay

```bash
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin --symbol AAPL --date 2019-12-30 --allow-truncated-tail
```

Output on the slice (exit status 0):

```text
Source
  messages 7844756: 6521447 other symbols, 1314891 unused types, 8417 events; locate 13
  truncated tail   yes (last partial message ignored)

Events 8417, 04:00:00.007075802 to 09:30:10.715766934 local
  Add              4672
  Modify           0
  Replace          58
  Cancel           10
  Delete           2998
  Execute          494
  Clear            0
  Trade            185

Applied 8417, rejected 0

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
  spread 0.0600, orders 1391, peak orders 1391, crossed no, locked no

Invariants at end: ok (0 periodic checks passed)
Digest            7f1e922654415cf3
```

The run also prints its configuration, and `Elapsed` and `Memory` lines. Several symbols, or every symbol, in one
pass, each into its own book:

```bash
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin --symbols AAPL,MSFT --date 2019-12-30 --allow-truncated-tail
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30
```

The second command is the full-day run of [Verified results](#verified-results). With the defaults (`--book list
--threads 2`) its replay loop takes about 54 s of wall time on the machine described under
[Performance](#performance), and about 77 s with `--threads 1`.

Once `chronos-serve` has built a day into its per-symbol store (see [Trading Terminal](#trading-terminal)), `replay`
and `backtest` read one symbol's day from the store in place of the day file, its own bytes alone:

```bash
./build/simulator replay --store data/itch/store --symbol AAPL --date 2019-12-30
```

### Backtest

```bash
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin --symbol SPY --date 2019-12-30 --allow-truncated-tail --strategy mm
```

Part of the output on the slice (exit status 0):

```text
Execution
  maker fills      143 (13693 shares)
  taker fills      0 (0 shares)
  fill rate        3.65% of 375000 shares quoted
  cancels          3612 done, 64 rejected (too late)
  would cross      1 rejected
  FIFO anomalies   0

Fill attribution (maker fills/FIFO anomalies; crosses = first second of 09:30, 16:00)
  rule, msg         pre-open    open cross       regular   close cross    post-close
  (a) E                 26/0           2/0           3/0           0/0           0/0
  (a) C                  0/0           0/0           0/0           0/0           0/0
  (d) E                  5/0           0/0           1/0           0/0           0/0
  (d) C                  0/0           0/0           0/0           0/0           0/0
  (c) Add               86/0           2/0          14/0           0/0           0/0
  (c) Replace            0/0           0/0           4/0           0/0           0/0

Portfolio
  position         -233 shares
  cash             $75289.4000
  realised PnL     $10.7069
  unrealised PnL   -$5.9369
  fees             $0.0000
  total PnL        $4.7700 (realised + unrealised - fees)
  max drawdown     $141.0200
  max |position|   468 shares
  open orders      2 at the end
```

Its history digest, `e9f436a5ac864f40`, equals that of `replay --symbol SPY` on the same file: the strategy's
orders did not change the replayed book.

Full-day runs: the market maker without fees (the row in [Verified results](#verified-results)), with a $0.0020
per share maker rebate, and the imbalance taker paying $0.0030 per share (the Reg NMS access-fee cap):

```bash
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy mm
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy mm --maker-fee-ticks -20
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy imbalance --taker-fee-ticks 30
```

### Real-data tests

```bash
CHRONOS_ITCH_FILE=data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin ./build/simulator_tests --gtest_filter='ItchRealDataTest.*:BacktestDeterminismTest.*'
```

`CHRONOS_ITCH_SYMBOL` defaults to `AAPL` and `CHRONOS_ITCH_DATE` to `2019-12-30`. Without `CHRONOS_ITCH_FILE` the
real-data tests skip. Use the slice: these tests read the file several times.

### Benchmarks

```bash
bench/run.sh [label]
```

It builds `Release` with `-DCHRONOS_BENCH=ON` in `build/bench`, replays the full day 3 times and the slice 5 times,
runs `simulator_bench` 5 times, profiles one more full-day replay with `sample` (macOS), and writes everything to
`bench/results/<date>-<commit>[-dirty][-label]/`. It needs both data files and takes about 5 minutes with the
defaults. `FLAGS` passes options to every replay (`FLAGS="--book ladder --threads 1" bench/run.sh ladder-1`), and
`FULL_RUNS`, `SLICE_RUNS` and `BENCH_REPS` set the counts (0 skips that part; `PROFILE=0` skips the profile).
`bench/summary.py <folder>...` prints and compares the tables.

### Trading Terminal

`chronos-serve` replays one day interactively, one symbol at a time, and serves the Trading Terminal, a React UI in
[`web/`](web/). Building the UI needs Node 24 and npm; the C++ build does not.

```bash
cmake --build build -j --target chronos-serve
(cd web && npm ci && npm run build)            # writes web/dist
./build/chronos-serve --data data/itch --date 2019-12-30
```

Open the URL it prints, `http://127.0.0.1:8080/`. Type a symbol such as `AAPL` in the top bar and press Enter: the
replay waits, paused, before the symbol's first event. Play at 1×, 10×, 100× or Max, step one event at a time, or
seek to a New York time. To replay the symbol over several days in a row, tick them in the day list beside the symbol
box before loading it: each night passes at once, at any speed, and Seek then takes a day and a time. Click a book level
to price the ticket, then trade post-only, IOC or market. Orders go through the backtests' `ExecutionSimulator`: 50 µs
to arrive, 50 µs for each report, a place in the L3 queue, no fees, and limits of 10,000 shares of position, 1,000
shares an order and 16 open orders. Each command is logged to `sessions/<id>.jsonl` in the working directory, so the
session can be reproduced. The first line on stdout names the URL and that log; errors go to stderr.

- Run it from the repository root, as `--web` defaults to `web/dist`.
- `--data` names a directory of days, each file named `MMDDYYYY.NASDAQ_ITCH50.bin`; `--date` picks the starting day,
  the latest by default. A new file is read once before serving, about a minute a day, for its XXH3-128 hash and the
  regular hours its own System Events set, which `runs/fingerprints.json` keeps, and in the same pass into the
  **per-symbol store**: one zstd file per symbol, `<SYMBOL>.itch.zst`, holding its frames and every System Event of
  each day, with an `index.tsv` of each segment's place, size and XXH3-128 hash. It takes about a third of the size of
  the days, in `<data>/store` unless `--store` names another, and one `chronos-serve` at a time uses it. A build
  needs free space of at least 60% of its day file; a day without it, or whose build fails, is skipped with a line on
  stderr saying why. Every replay of a symbol, the terminal's, a Lab run's, Verify's and Watch's, reads only that
  symbol's segments, so a load or a restart costs the symbol's bytes, not the day's. The symbol box suggests the
  symbols with a market event on every day picked. `GET /api/days` lists the days whose store is built; a day file
  that changes is built again at the next listing. A store file damaged after its build fails its checks when read,
  naming the symbol and day, and is not rebuilt on its own: delete `<store>/index.tsv` and restart to build every day
  again.
- Options: `--host`, `--port` (default 8080), `--web`, `--token`, `--fsync`, `--allow-truncated-tail`, `--store` and
  `--book`;
  `./build/chronos-serve --help` lists them.
- The slice needs a day's file name, so link it into a directory of its own:
  `mkdir -p data/slice && ln -s ../itch/12302019.NASDAQ_ITCH50.first-256MiB.bin data/slice/12302019.NASDAQ_ITCH50.bin`,
  then pass `--data data/slice --allow-truncated-tail`.
- Any `--host` other than `127.0.0.1` needs a token. `chronos-serve` prints one if `--token` is not given, and the
  terminal asks for it once per browser tab.

**Developing the UI.** Vite serves `web/src` with hot reload. It proxies `/api` and `/ws` to `chronos-serve` and
rewrites `Host` to `127.0.0.1:<port>`, and the `Origin` of the WebSocket and of the Lab's POSTs and DELETEs too when the
page itself is on `localhost` or `127.0.0.1`: without a token, that is all `chronos-serve` accepts. Any other site's
`Origin` goes through unchanged and is refused.

```bash
./build/chronos-serve --data data/itch --date 2019-12-30 --port 8080
(cd web && CHRONOS_PORT=8080 npm run dev)      # http://localhost:5173/
```

**Tests.** From `web/`:

```bash
npm run typecheck && npm test                  # Vitest: the stores, candles, order checks, the Lab and formatting
npx playwright install chromium                # once
npm run e2e                                    # builds web/dist, then Playwright; needs the build's two binaries
```

Each end-to-end test starts its own `chronos-serve` on two small generated days, which `chronos-e2e-fixture` writes to a
temp directory. The tests place an order and see it fill; drop the WebSocket in the middle of a restart and check that
the terminal ends on the new generation and sends nothing stale; seek to a New York time; load both days and play across
the night; and connect to a server that wants a token. The binaries are `chronos-serve` and `chronos-e2e-fixture`, from
`build/` or the directory `CHRONOS_BUILD` names.

The Lab's tests run on the days `chronos-e2e-fixture DIR --lab` writes: on each, the default market maker makes $1 a
cycle on ACME, buying by rule (a) and selling by rule (c), two cycles on 2019-12-27 and three on 2019-12-30, which
alone lists ZETA, with one bid, and IDLE, with no market event. They run one day and two, compare the two, Watch a run
in the terminal at Max and see it reproduced, Verify, Export and Delete a run, see ZETA refused on 2019-12-27 and IDLE
left out of the suggestions, and Stop watching a Watch, which Restart and Seek keep. They also edit Monday in place
after an ACME run: an edit of ACME's directory entry changes its segment, so Verify and Watch are refused, while an
edit of ZETA's leaves ACME's segment as it was, so both reproduce the run.

**Interactive benchmark** (`chronos-serve` at max speed, with the terminal attached and trading):

```bash
CHRONOS_BENCH_FILE=../data/itch/12302019.NASDAQ_ITCH50.bin CHRONOS_BENCH_SYMBOL=AAPL npx playwright test bench
```

`CHRONOS_BENCH_FILE` names the ITCH file; a cut slice works. `CHRONOS_BENCH_SYMBOL` (default `AAPL`) is the stock to
replay. `CHRONOS_BENCH_DATE` (default `2019-12-30`) is the file's day, which `chronos-serve` needs for its name.
`CHRONOS_BENCH_STORE` names the per-symbol store to serve from, such as `../data/itch/store` when no other
`chronos-serve` is using it: a day it already holds is read once to check its hash, and nothing is written. Without
it, each run first builds the day into a temp store, about a minute and a third of the day's size on disk.
`CHRONOS_BENCH_OUT` names a file to write the JSON summary to; it is always printed.

The figures below were recorded before the per-symbol store, when every replay read the whole day file. One run on
2026-10-17, on the development machine (Apple M4, `RelWithDebInfo`): AAPL's full day at Max, with the terminal
attached and sending 39 one-share market orders, replayed 1,519,358 AAPL events in 16.3 s, about 93,000 a second; the
source then read all 268,744,780 messages of the day to find them, where it now reads AAPL's segment alone. The page
received 18 snapshots a second and the server dropped none (`dropped_snapshots`), the command queue was at most 1
deep, and the page drew 60 frames a second, its slowest frame 17 ms. The snapshot metrics carry no per-event latency,
so there is no p50 or p99 per event yet.

**Configuration 4** (a Lab run in flight): `CHRONOS_BENCH_LAB=SPY:market_maker` also queues that Lab run, over the same
day with a Full-day window, as the replay starts, and runs it again on the idle server once the replay has ended; the
summary adds both wall times. Three back-to-back pairs on 2026-10-20, as above (load average 3 to 6): AAPL replayed at
73,773 to 77,427 events a second without a Lab run and 74,519 to 76,875 with the SPY market maker's day in flight, and
the run took 19.6 to 20.3 s while the terminal played, 21.4 to 22.6 s on the idle server. Neither slowed the other
beyond the runs' own spread. Both then read the same day file and shared its page cache; each now reads only its own
symbol's segments.

### Strategy Lab

`chronos-serve` also runs backtests in the background, one at a time on a worker thread, while the terminal plays:
a built-in strategy over one symbol and one or more days, at full speed. The **Strategy Lab** page, at
`http://127.0.0.1:8080/#lab` or through the switch in the top bar, runs and studies them:

- **New run** (left): the strategy and its params, the symbol, the days (a day whose regular hours are the 09:30-16:00
  fallback is marked), the window, latencies, fees in dollars a share, and limits; an empty limit follows the params.
  The defaults and caps are the server's. A value past its cap, or one the server refuses, is marked, and a symbol a day
  does not list names the day.
- **Results** (middle): total PnL, fills, fill rate, max drawdown, fees and rebates, max |position|; the PnL at every
  second with a market event, its drawdown below; the fills with their rule and queue ahead at entry, the fill
  attribution, the PnL by hour and, over several days, by day. A failed or cancelled run's figures are marked partial.
- **Runs** (right): each run's state, progress or PnL. Tick two to **Compare**: their PnL overlaid, the second
  dashed and lined up by day and time of day, their stats side by side, every setting that differs marked, and runs
  over another symbol or other days flagged. For the run picked: **Watch in Terminal** replays it in the terminal,
  paced, and says when the replay ends whether it reproduced; **Verify** re-runs it on the server; **Export JSON**
  saves it; **Edit as new** copies its settings into the form; **Cancel** and **Delete**. Its provenance lists the
  build, each day's hash, session source and size, the engine, the timings, the digests and every verification.

The page polls the runs about once a second while one is queued or running. Its HTTP API:

| Endpoint | Does |
|---|---|
| `GET /api/strategies` | the strategies, their params, defaults and caps, the windows, `max_days` and `max_queued` |
| `POST /api/backtests` | queues a run; 202 `{"id"}`, 400 `{"error","field"}` or `symbol_missing` with its `days`, 429 when 8 wait |
| `GET /api/backtests` | every run, without its results: state, progress, PnL and fills |
| `GET /api/backtests/<id>` | the whole run: spec, provenance, results |
| `POST /api/backtests/<id>/cancel` | cancels a queued or running run; 409 once it has ended |
| `DELETE /api/backtests/<id>` | removes a run and its file (200 with a `warning` if the file stays); a running one stops and is never saved |
| `POST /api/backtests/<id>/verify` | re-runs a completed run and records the verdict; 202 (a repeat returns the job already queued or running), 404, 409 if partial, if a day's segment changed, if recorded before the per-symbol store (`before_store`) or if run with another `--allow-truncated-tail`, 429 if the queue is full |

A run is posted as `{"symbol":"SPY","days":["2019-12-30"],"strategy":"market_maker"}`. Every other key takes
`simulator backtest`'s default: `params`, `latency` (`order_ns`, `report_ns`), `fees` (`maker`, `taker`, in ticks a
share), `limits` and `window`. An omitted limit follows the params, as on the command line. The window is `"regular"`
by default, so a strategy trades only inside each day's feed session, from the day's own 'Q' to its 'M' System Event
(09:30-16:00 when a file lacks them); `"full_day"` trades the whole file. A one-day, Full-day run with the defaults gives
`simulator backtest`'s PnL, fills and fill attribution.

- Each finished run is saved to `runs/<id>.json` in the working directory, with the build it ran on (commit, source
  digest, compiler), each day's hash, its symbol's segment (`segment: {hash, size}`) and regular hours, and its
  results: the PnL curve at the end of every second with a market event, PnL by day and by hour, the first 50,000
  fills and the fill attribution. A restart reloads them.
- **Verify** re-runs the run, reading its symbol's segment of each day afresh from the store and checking it as read.
  The segments are the run's identity: another symbol's change to a day file leaves them, and the verdict, as they
  were. Each verification is recorded with its verdict: `reproduced` (the same digests), `mismatch`, `different_code`
  (this build's source digest is not the run's, so nothing is proven either way) or `input_changed` (a segment is not
  the one the run read: its day was built again from a changed file, or the store file no longer reads back intact).
  A run's `reproduced` turns true once a verification on the same code and segments has matched, and false if one did
  not. A run saved before the per-symbol store has no segment hashes: it stays listed, but Verify answers 409
  `before_store` and Watch is refused; Edit as new queues it again.
- **Watch in Terminal** replays a completed run in the terminal, paced, at any speed: the WebSocket control
  `{"type":"control","action":"watch","run_id":"<id>"}`, from the controlling tab. The server loads the run's spec
  and days, refuses a run whose segments have changed since, and pauses before the first event. When the replay ends it
  sends `report{kind:"reproduced"}` with the verdict, by the same rules as Verify; a command applied during the Watch
  makes it another run, so the verdict is then `mismatch`. The Watch's command log names the run and reproduces it.
  Restart and Seek replay the Watch again; **Stop watching**, in its banner, loads the plain replay of the same symbol
  and days in its place.
- A POST or DELETE must be `Content-Type: application/json` (else 415) and carry the `Origin` of the page this server
  served (else 403), so no other site can queue or delete a run; with `--token` it also needs the `Authorization` header.
  A body over 64 KiB closes the connection.

## How it works

```text
ITCH 5.0 file ─► MappedFile ─► ItchSource ── R messages ──► InstrumentRegistry (SymbolId → scales)
                  (mmap)        locate filter, then decode A F U X D E C P
                                       │ MarketEvent (48 bytes) through MarketEventSource::next()
                                       │ (CsvSource and VectorSource implement it too)
            ┌──────────────────────────┴──────────────────────────┐
            ▼ replay                                              ▼ backtest
    MarketReplay::run                              EventLoop<Strategy>, per market event e:
            │                                        ① ExecutionSimulator::on_market(e)   pre-event
            ▼                                        ② MarketBook::apply(e)   the only writer
    MarketBooks: a MarketBook per symbol             ③ Portfolio::mark
    on one shared BookStorage (the history)          ④ Strategy::on_market(e)             post-event
                                                   our orders, through SimQueue:
                                                     Strategy ─► OrderGateway ─► ExecutionSimulator
                                                     ExecutionSimulator ─► Portfolio, Strategy
```

| Path | Contents |
|---|---|
| `include/simulator/types/` | `Price`, `Quantity` (`uint64`), `Timestamp` (ns since the Unix epoch, UTC), `OrderId`, `SymbolId` (`uint32`), `Side` |
| `include/simulator/orders/` | `Order`, `PriceLevel`, `OrderBook` (a list or a ladder of levels), `OrderIndex` and `LevelIndex` (open addressing), `OrderPool`, `LevelPool`, and `BookStorage`, which reserves them all up front: the internals of `MarketBook` |
| `include/simulator/market/` | `MarketEvent`, `InstrumentRegistry`, `MarketEventSource`, `VectorSource`, `MarketBook`, `MarketBooks`, `MarketReplay`, and `SpscRing` and `PipelineSource`, which decode on a thread of their own |
| `include/simulator/io/` | `MappedFile` |
| `include/simulator/itch/` | `ItchSource` with its framing (`ItchMessageReader`, `ByteReader`) and `ItchDecoder` |
| `include/simulator/csv/` | `CsvSource`, a CSV event source for the library and tests (the CLI reads ITCH) |
| `include/simulator/sim/` | `SimEvent` and `SimQueue`, `OrderGateway`, `ExecutionReport`, `ExecutionSimulator` |
| `include/simulator/portfolio/` | `Portfolio`, `RiskLimits`, `FeeSchedule` |
| `include/simulator/engine/` | `EventLoop<S>`, the `Strategy` concept, `StrategyContext`, `NullStrategy` |
| `include/simulator/strategies/` | `ImbalanceTaker` and `MarketMaker`, both toys |
| `include/simulator/metrics/` | `LatencyHistogram` |
| `include/simulator/cli/` | argument parsing and the usage text |
| `src/` | the implementations, mirroring `include/simulator/` |
| `app/` | `main.cpp`, and `alloc_counter.cpp`, a counting `operator new` linked only into executables |
| `bench/` | `simulator_bench.cpp`, `run.sh`, `summary.py`, [`RESULTS.md`](bench/RESULTS.md) and `results/` |
| `tests/` | one suite per component; `tests/support/` holds the source conformance suite; `tests/itch/` holds a byte-exact ITCH builder and real NASDAQ message bytes it is checked against |

### D1: history and simulation never mix

- `MarketBook` is the historical L3 book of one symbol. `MarketBook::apply` is its only mutator, and only the two
  loops below call it, with market events.
- The execution simulator, the portfolio and the strategies get `const` references to the book. Our orders live in
  `ExecutionSimulator` as shadow orders and are never inserted into `MarketBook`. Historical orders are never matched
  against each other.
- So a backtest leaves the same history digest as a plain replay of the same file, which
  `BacktestDeterminismTest` asserts.

### D3: one deterministic loop

- `EventLoop<S>` merges the market stream with `SimQueue`, which holds only our events (`OrderArrive`,
  `CancelArrive`, `ReportDeliver`, `Timer`). Events run in (timestamp, stream, sequence) order, and a tie goes to
  the market event. That is pessimistic on purpose: an order that arrives at `t` queues behind every historical Add
  stamped `t`.
- Per market event, in this order: ① `ExecutionSimulator::on_market` on the pre-event book, because the fill rules
  need the historical order that the event references; ② `MarketBook::apply`; ③ `Portfolio::mark` while a position
  is open; ④ `Strategy::on_market`, only if the book applied the event, on the post-event book.
- Two constant latencies, 50,000 ns each by default: the order latency (strategy to exchange, which also stands for
  the market-data delay) and the report latency (exchange back to the strategy and the portfolio). An order sent in
  ④ arrives after the event that triggered it, so there is no look-ahead.
- At the end of the market stream, the events still in `SimQueue` are drained against the final book, at most
  100,000 of them. New timers are refused from then on.
- The loop never reads a wall clock. The same input and flags give the same output.
- `replay` runs `MarketReplay::run`, a market-only loop with the same counting and checks.

### D2: the fill model

From the comment in [`include/simulator/sim/execution_simulator.hpp`](include/simulator/sim/execution_simulator.hpp):

- **IOC**, at arrival: walk the visible opposite levels best first while they cross, and fill
  min(remaining, level quantity) at each level's price as taker. Whatever is left expires. Fills take no
  liquidity from the book (no market impact).
- **Day orders are post-only.** One that would take liquidity is rejected (`WouldCross`). Otherwise it rests
  virtually: `anchor_sequence` = the book's last sequence and `ahead_quantity` = the visible quantity at its price,
  so every historical order already at that price is ahead of it.
- **On each market event**, before the book applies it, for each of our resting orders `o`:
  - **(b)** an Execute, Cancel, Delete or Replace of a historical order ahead of `o` at `o`'s price shrinks
    `ahead_quantity`;
  - **(a)** an Execute of a historical order behind `o` at `o`'s price fills `o` at its price;
  - **(d)** an Execute of a same-side order at a worse price fills `o`: a trade-through would have hit `o` first;
  - **(c)** an Add, or a Replace's new order, that crosses `o` from the opposite side fills `o` at its price.
- **Trade-price guard.** Rules (a) and (d) fill `o` only if the trade price is not better than `o`'s price. The
  trade price is the execution price of an ITCH `C`, else the historical order's price.
- One event fills our orders in our priority order (best price, then arrival), and the total is capped at the
  event's quantity. Every such fill is maker liquidity.
- **Diagnostics.** A rule (a) or (d) fill while `ahead_quantity > 0` counts as a FIFO anomaly, and the first 10 are
  kept with their context. `backtest` prints maker fills and anomalies by rule, message type and session phase.
- **Portfolio.** Reports are booked at delivery, `report latency` after they happened, just before the strategy sees
  them. Longs are marked at the best bid and shorts at the best ask. Fees are per share in price ticks; a negative
  fee is a rebate. Money is integer: tick-shares ($0.0001) for ITCH, with `__int128` products. PnL is realised +
  unrealised − fees.

### Source-agnostic events

- `MarketEvent` is 48 bytes: timestamp (`int64` ns since the Unix epoch, UTC), order id, new order id, price
  (`int64` ticks at the instrument's price scale), quantity (`uint64`), `SymbolId`, type and side.
- Eight types: Add, Modify, Replace, Cancel, Delete, Execute, Clear and Trade. ITCH maps `A`/`F` to Add, `U` to
  Replace, `X` to Cancel, `D` to Delete, `E`/`C` to Execute (`C` carries its execution price) and `P` to Trade,
  which the book ignores. Modify and Clear exist for future sources; `MarketBook` rejects them as `Unsupported`.
- `ItchSource` reads the locate field before decoding, so messages of unselected symbols are never decoded. The
  `R` message of each selected symbol registers its instrument (venue XNAS, price scale 4). ITCH times are ns after
  midnight; the source adds the session midnight, from `--date` and the US Eastern offset of that date, or
  `--utc-offset`.
- Every source must pass `SourceConformanceTest`: timestamps never go backwards, references name live orders of an
  earlier Add, sizes stay within the remaining quantity, symbols are registered first, the same bytes give the same
  events, and errors are sticky with a position. It is instantiated for `VectorSource`, `CsvSource` and `ItchSource`.

### How correctness is checked

- `ItchBuilderTest.*MatchesRealNasdaqBytes`: the test-side ITCH builder reproduces real NASDAQ messages byte for
  byte, so every ITCH test runs on the real wire layout.
- `MarketBookTest.RandomStreamKeepsInvariantsAndIsDeterministic`: 10,000 seeded random events of every type, valid
  and invalid. The invariants hold after every event, a rejected event never changes the digest, and two runs end
  with the same digest.
- `ExecutionSimulatorTest.WorkedExample1SweepsThenExpiresAndRepeatsIdentically`,
  `RestingOrderTest.WorkedExample2QueuePosition`, `RestingOrderTest.WorkedExample3CrossingAddFillsAtOurPrice` and
  `EventLoopTest.TieGoesToMarketInTheSpecWalkthrough` pin the fill rules and the loop order with hand-worked
  examples.
- `BacktestDeterminismTest.*`: two backtests are identical and leave the history digest of a plain replay;
  synthetic always, and on real data when `CHRONOS_ITCH_FILE` is set.
- `ItchRealDataTest.*`: one symbol and every symbol of a real file, with 0 rejections and periodic invariant
  checks.
- The full-day replay in [Verified results](#verified-results).

## CLI reference

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
  --book B                the price levels: list (default), each side a list found through an index;
                          or ladder, which adds a window of 256 price units around each side's best
  --threads N             2 (default): one thread decodes, one applies; 1: one thread does both
  --json PATH             also write the results, metrics, build and input as JSON;
                          backtest adds its config, fills, fees and PnL

replay only:
  --symbols A,B,C         several symbols in one pass, each into its own book
  --all                   every symbol in the file
  --latency               time every batch of 1024 events; print ns/event percentiles
                          over the batches
  --latency-batch N       events per timed batch (implies --latency)

backtest runs a toy strategy against the replayed book, which our orders never change:
  --strategy S            imbalance (default): IOCs on top-of-book imbalance;
                          mm: post-only quotes at the best bid and ask
  --order-latency-ns N    strategy to exchange, in nanoseconds (default 50000)
  --order-latency-us N    the same in microseconds
  --report-latency-ns N   exchange back to strategy and portfolio (default 50000)
  --report-latency-us N   the same in microseconds
risk limits (the largest order is the strategy's own --clip or --quote-size):
  --max-position N        |position| limit in shares (default 500)
  --max-open-orders N     live orders at once (default 1 for imbalance, 2 for mm)
fees, per share in price ticks ($0.0001 for ITCH):
  --taker-fee-ticks N     paid when taking liquidity (default 0)
  --maker-fee-ticks N     paid when providing it; negative = rebate (default 0)
imbalance only:
  --threshold X           trade when one top-of-book size >= X times the other (>= 1, default 3)
  --clip N                shares per order (default 100)
  --cooldown-ms N         wait after each order (default 100)
mm only:
  --quote-size N          shares per quote (default 100)
  --requote-ms N          a quote lives at least this long before it is moved (default 100)

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
```

A bad argument prints a two-line error on stderr and exits 2. `--json PATH` writes one JSON object with
`"schema": 1` and the members `build`, `machine`, `input`, `run`, `result`, `time`, `memory` and `latency`;
`backtest` adds a `backtest` member. `bench/summary.py` reads these files.

## Limitations and assumptions

- **No market impact.** Our fills never remove historical liquidity, so repeated IOCs can take the same visible
  shares again. Fill rates for aggressive orders are optimistic.
- **L3 data only.** Every source emits per-order events. The CLI reads NASDAQ TotalView-ITCH 5.0; `CsvSource` is
  available to the library and tests only.
- **Order types:** IOC and post-only Day orders. A marketable Day order is rejected, not partly filled.
- **The fill rules are (a), (b), (c) and (d) with the trade-price guard.** Hidden and non-displayed liquidity is
  invisible: ITCH `P` prints never fill our orders, and neither do cross trades (`Q`). Opening and closing crosses
  and trading halts are not modelled in fills.
- **Constant latencies.** The order latency also stands for the market-data delay. There are no latency
  distributions.
- **One symbol per backtest.** `replay` takes one symbol, a list or all of them; `backtest` takes one.
- **Fees are per share only:** maker and taker fees or rebates. Regulatory fees are not modelled yet: SEC Section 31
  and the FINRA TAF on sells.
- **Session times.** Regular trading hours are fixed at 09:30 to 16:00 ET, so half-day sessions are not handled. The
  default US Eastern offset follows the daylight-saving rules in force since 2007; pass `--utc-offset` for earlier
  dates.
- **Platforms.** POSIX only (`mmap`). Built and measured only on macOS on an Apple M4. Linux is unverified until the
  CI workflow runs.
- **Sanitizers.** ASan has not run (see the [ASan note](#build-and-test)); UBSan is clean.
- **The digest** is a regression check, not a proof (see [Verified results](#verified-results)).
- **The strategies are toys,** written to exercise the fill model, not to make money.

## Status and roadmap

**Built:**
- The source-agnostic event contract, the source conformance suite, and `VectorSource`, `CsvSource` and
  `ItchSource`.
- The L3 `MarketBook`, with invariant checks and digests. Multi-symbol replay over a shared order pool.
- The full-day replay of every symbol with 0 rejections.
- The benchmark harness, a profile, and two profile-driven optimisations (see [`bench/RESULTS.md`](bench/RESULTS.md)).
- The strategy layer: event loop, latency, fill model, portfolio with risk limits and fees, two toy strategies.
- The CLI with `--help`, `--version`, documented exit codes and `--json`.
- The Trading Terminal: `chronos-serve` (HTTP and WebSocket, a command log, reconnect and resync) and a React UI with
  a TradingView-style chart, L3 queue positions and Playwright end-to-end tests.
- Days chained in the terminal, and the Strategy Lab: backtests over one or more days run from the UI, kept,
  compared, verified, and watched in the terminal to a digest-checked verdict.
- The per-symbol store: each symbol's days as zstd segments, built once per day, so every replay of a symbol reads its
  own bytes, about 1% of a day, checked against their hash.
- ITCH trading status (`H`): a halted book may cross until trading resumes, and the invariant check knows it.
- Price levels in a preallocated pool, as a list or a ladder (`--book`), zero allocations in the replay, and a
  two-thread decode → apply pipeline (`--threads 2`).
- A GitHub Actions workflow and `.clang-format` / `.clang-tidy` configuration.

**Not yet:**
- **Regulatory sell fees:** SEC Section 31 and FINRA TAF.
- **More performance work, each gated on the profile:** placing a new price level, now 44 % of the book thread's
  time, by a ladder whose window counts exchange ticks (today's counts price units: 256 of them are $0.0256 in ITCH,
  about one cent level per side), then cheaper ITCH decoding.
- **LOBSTER:** a second real adapter, and a check of `MarketBook` against LOBSTER's own book after every message.
- **`MergedSource`:** a k-way merge by timestamp, for multi-venue or multi-day replay.
- **Replay through `EventLoop`:** run the replay through `EventLoop` with a null strategy once its throughput is
  shown to match `MarketReplay::run`.
- **CI and formatting:** a first green CI run, then one formatting-only commit with `.clang-format`.
- **Open question:** CSV input in the CLI.
