# Testing, validation and acceptance

How each layer is validated, and the acceptance criteria for the system with their status. Commands, suite sizes and
CI jobs are in [testing.md](../testing.md).

## Validation by layer

| Layer | What is validated | How |
|---|---|---|
| **Domain semantics** | prices, quantities, timestamps and their conversions; pools and indices; the portfolio's arithmetic, marks, fees and risk checks; the gateway's limits and latencies | unit tests per type and component |
| **Readers and decoding** | framing, every translated ITCH message, field offsets, symbol filtering, instrument registration, clock conversion, truncated files, every error code | unit tests on messages built by a test-side ITCH builder that is proved byte-exact against real NASDAQ messages; the same for CSV |
| **The source contract** | non-decreasing times, live references, quantities within the remaining, registration before use, repeatability, sticky errors with a position | one conformance suite run over the ITCH, CSV and in-memory sources |
| **Book reconstruction** | each event type's effect and every rejection; the invariants; both level backends | a model test that applies a random operation stream to the list, the ladder at two steps, and a plain ordered-map reference, and compares them after every operation; 10,000 seeded random events with invariants checked after each; transition tests for every ladder window edge and recentring |
| **Replay integration** | routing to many books, capacity exhaustion, out-of-order times, source errors, two threads equal to one | replay tests over generated streams; an end-to-end ITCH test that replays symbols registered mid-stream on two threads and one |
| **Execution and fills** | the IOC sweep, resting, cancels, each fill rule, the guard, priority among our orders, halts, the loop's ordering at a tie | worked-example tests, one per rule, with every intermediate state; the strategies' rules on generated days |
| **Determinism** | identical runs; history unchanged by simulation; reproduction from logs and saved runs | two backtests compared byte for byte, and against a plain replay's book digest; command-log reproduction; Lab verify and watch; golden digests for the fixture runs; the same digests on Linux/g++ and macOS/clang |
| **Server and engine** | admission, ordering at one timestamp (market, simulator, command), command batches, pausing with latency, end of stream, generations, snapshots, the report ring, gap and resync, the log's failure | session, channel, peer and in-process server tests, under ThreadSanitizer in CI |
| **Isolation** | the engine includes and links no server or UI code; no allocation on the replay path | two scripts in CI |
| **Overload and failure** | a client that stops reading; a full queue; a failed log; a stale generation; a second tab; rate limits; malformed and oversized messages; a damaged store segment | peer and server tests; store tests |
| **The browser** | the stores, candles, the ladder, the PnL curve, symbol matching, order checks, the Lab's model; the terminal and the Lab end to end against a real server | Vitest unit tests; Playwright end-to-end tests on generated day files |
| **Performance** | throughput, cost per event, allocations, memory, against a previous build | `bench/run.sh` full-day and slice runs, micro-benchmarks, a profile, a written selection rule ([performance-and-determinism.md](performance-and-determinism.md)) |
| **Real data** | every symbol of a real day applies with 0 rejections; backtests on it are identical; every store segment replays to the day file's book | tests that run when a day file is given (skipped otherwise, and in CI, which has no market data); the recorded full-day correctness runs |

Every sanitized build aborts on the first report, so a passing sanitized test is a clean one. On the day this
document was written (2026-10-28), at commit `9f6b138`, `ctest` ran 698 tests: 692 passed and 6 were skipped (five
real-data tests without a day file, and one conformance check that does not apply to the in-memory source). With the
strategy plugins and scripts (2026-10-29), `ctest` ran 723: 717 passed and the same 6 were skipped.

## Acceptance criteria

Statuses: **Pass** (a named test, CI check or recorded run shows it), **Partial** (part of it is shown; the gap is
named), **Pending** (not yet built or measured).

### Replay and reconstruction

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A1 | Market events are processed in non-decreasing time order; an earlier time stops the run and is not applied | **Pass** | the replay's out-of-order test; the conformance suite's time check |
| A2 | Invalid input produces an explicit failure with its position, never silent corruption | **Pass** | reader error tests for every error code; sticky errors in the conformance suite; truncated-file tests |
| A3 | An event that does not fit the book is rejected with a reason and leaves the book unchanged | **Pass** | the random-stream test: a rejected event never changes the digest |
| A4 | Every event of NASDAQ's full day of 2019-12-30 applies: 0 rejections, every book empty at the close, invariants passing | **Pass** | the recorded correctness runs in `bench/RESULTS.md`; re-runnable with the real-data tests |
| A5 | Both level backends, and one thread and two, give the same books | **Pass** | the model test after every operation; the same full-day digest in every configuration; the two-thread end-to-end test |
| A6 | A full structure stops the run cleanly, naming it, without growing | **Pass** | exhaustion tests for every pool and index |
| A7 | Replaying a symbol's store segment gives the same events and book as the day file | **Pass** | the store's equivalence test on generated days; every symbol of the real day when present |
| A8 | The rebuilt book matches an independent reference after every message | **Pending** | LOBSTER ([D19](decisions.md#d19-a-second-real-source-lobster)) |

### History and simulation

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A9 | Historical replay never matches historical orders or produces simulated trades because historical orders cross | **Pass** | the replay has no simulator; the book has no matching; a crossed book is a reported invariant break while trading in regular hours, and allowed while halted |
| A10 | A backtest leaves the same historical book as a plain replay | **Pass** | the determinism tests, on generated data always and on a real day when present |
| A11 | Each fill rule, the trade-price guard, the IOC sweep and the ordering at a tie behave as specified | **Pass** | the worked-example tests |
| A12 | No look-ahead: an order sent on an event arrives no earlier than the order latency after it | **Pass** | loop tests with latencies, including across a quiet period |
| A13 | The fill rules are FIFO-consistent on real data | **Partial** | 0 anomalies in the SPY market maker's full day at `a5ed53b` (2026-10-13), recorded in [backtesting.md](../backtesting.md); not re-run since the off-grid rule, and no test asserts it |
| A14 | Fill rates and PnL are reported with rule (c) on and off, and across latencies | **Pending** | [Q1](README.md#open-questions-and-known-gaps) |

### Determinism and reproduction

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A15 | Repeated runs on the same input, configuration and build give the same digests | **Pass** | the determinism tests; golden digests; the full-day digest across configurations |
| A16 | The same digests on different compilers and hosts | **Pass** for Linux/g++ against macOS/clang on the 256 MiB slice | [testing.md](../testing.md#how-correctness-is-checked) |
| A17 | An interactive session reproduces from its command log, digest for digest | **Pass** | the command-log reproduction tests |
| A18 | A Lab run reproduces on Verify and Watch, every fill in order; a run on other code or changed input says so | **Pass** | the Lab and Watch tests |
| A19 | The digest proves prices, not only counts, at the end of a day | **Pending** | a rolling digest is proposed ([Q2](README.md#open-questions-and-known-gaps)) |

### The server and the UI

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A20 | A session that drives the event loop gives the same loop report, book and portfolio as the headless run of the same strategy | **Pass** | a session test against the headless loop; a Lab backtest equals the CLI's backtest for every strategy |
| A21 | A slow or stalled client does not block the engine or other clients | **Pass** | an in-process server test with a client that stops reading; see [Q3](README.md#open-questions-and-known-gaps) |
| A22 | Ordering at one timestamp is market, then simulator, then command; a command batch's zero-latency orders run after the whole batch | **Pass** | session ordering tests |
| A23 | A full command queue, a failed log, a stale generation, a viewer, a rate limit and a malformed message each get their defined refusal | **Pass** | peer and server tests |
| A24 | A reconnecting client recovers its records from the ring, or gets a gap and a full state beyond it | **Pass** | peer resync tests |
| A25 | Headless executables do not depend on the server or UI | **Pass** | the isolation check in CI |
| A26 | Headless digests are unchanged by the UI work | **Pass** | the full-day digest unchanged; backtest outputs identical when the loop became step-wise |
| A27 | Headless performance is within run-to-run spread after the UI work | **Partial** | the loop benchmark within spread at that change; no full-day matrix of the server's addition ([Q7](README.md#open-questions-and-known-gaps)) |
| A28 | A client's memory in the server is bounded | **Pending** | [Q4](README.md#open-questions-and-known-gaps) |
| A29 | In a browser: load a symbol, play, place an order, see it fill; run, compare and verify in the Lab | **Pass** | the Playwright end-to-end tests |
| A30 | The page keeps 60 frames a second watching a run at Max | **Pass** on the measurement machine | the page-lag measurement in [ui.md](../ui.md#measuring-the-page) |

### Performance

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A31 | No allocation during the replay | **Pass** | 0 counted over the full day; the CI hot-path check; heap blocks unchanged on macOS ([Q9](README.md#open-questions-and-known-gaps)) |
| A32 | The default configuration is faster than the previous engine by more than the run-to-run range | **Pass** | the selection rule's output in `bench/RESULTS.md` |
| A33 | Per-event cost is known in distribution | **Partial** | batch-mean percentiles only ([Q8](README.md#open-questions-and-known-gaps)) |
| A34 | Throughput is reproduced on a second machine or architecture | **Pending** | one machine measured |

### Strategies

| # | Criterion | Status | Evidence |
|---|---|---|---|
| A35 | A strategy added as one file is offered by the CLI, the server and the Lab with no other change | **Pass** | `strategies/trend_taker.cpp` was added alone and runs from `--strategy`, `/api/strategies` and the Lab's form |
| A36 | Every plugin is deterministic and leaves the history digest of a plain replay | **Pass** | `StrategyRegistryTest`, over every registered strategy |
| A37 | Moving the strategies behind the registry changes no result | **Pass** | the same fills, PnL and books for the three strategies on AAPL and the market maker on SPY, 2019-12-30; the golden digests unchanged |
| A38 | A strategy written in the browser makes the fills the same logic makes as a plugin | **Pass** | `ScriptStrategyTest` on a synthetic market; the Lab on SPY's full day: the same 48,643 fills, PnL and digests |
| A39 | A script's run reproduces from its own source, after the script changes | **Pass** | `ServerTest` verifies a run after its script is edited; `WatchTest` watches a script's run to reproduced |
| A40 | A script cannot reach files, the clock or the network, and a runaway stops at the same place each run | **Pass** | `ScriptStrategyTest`: the missing libraries, fixed seeds across states, the instruction and memory budgets |
| A41 | Writing, checking, running and deleting a strategy works in the browser | **Pass** | the Playwright test of the Strategies page and the Lab |

## What a new change must pass

Before a change is handed off: the full C++ suite, the formatting check over every file, the isolation and hot-path
checks, and for web changes the type check, the unit tests, the build and the end-to-end tests. An engine change also
runs the full-day correctness gate when the day file is present, and a change meant to be faster runs the interleaved
benchmark ([performance-and-determinism.md](performance-and-determinism.md#rules-for-a-performance-claim)).
