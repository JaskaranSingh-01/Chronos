# Testing and CI

## The suites

| Suite | Count | What |
|---|---|---|
| `simulator_tests` | 518 pass, 6 skip | the engine: types, ITCH framing and decoding, sources, the book, pools and indices, the ladder, the pipeline, the event loop, the fill model, the portfolio, the strategies and their registry, the CLI's arguments |
| `chronos_server_tests` | 176 | the server: protocol schemas, peers and resync, sessions and generations, the command log and its reproduction, the store's builder and reader, the Lab, Watch, scripts written in the browser and their sandbox, security |
| `alloc_counter_test` | 3 | the counting `operator new` the replay's zero-allocation figure relies on |
| CLI tests | 19 | `simulator` and `chronos-serve` run as processes: exit codes, usage, a store shared between them |
| Vitest (`web/`) | 126 | the stores' reducers, candles and fill marks, the depth ladder, the PnL curve, symbol matching, order checks, the Lab's model and formatting |
| Playwright (`web/e2e/`) | 14, plus 2 measurement specs | the terminal and the Lab in Chromium against a real `chronos-serve` |

`ctest` runs 723 tests: every GoogleTest case, the CLI tests and a build-info test. Five of the 6 skips are the
real-data tests, which need a day file; the sixth is the conformance suite's error check, which does not apply to
`VectorSource`.

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
(cd web && npm ci && npm run typecheck && npm test && npm run e2e)
```

## How correctness is checked

- **Byte-exact ITCH.** `ItchBuilderTest.*MatchesRealNasdaqBytes`: the test-side ITCH builder reproduces real NASDAQ
  messages byte for byte, so every ITCH test runs on the real wire layout.
- **Every source behaves.** `SourceConformanceTest` runs one contract over `VectorSource`, `CsvSource` and `ItchSource`.
- **Every strategy plugin behaves.** `StrategyRegistryTest` runs every registered strategy, so a new plugin with no test
  of its own is still checked: a sound declaration (its name, its params' defaults within their bounds, positive
  limits that its defaults can trade under), two identical runs over a synthetic market, and the history digest of a
  plain replay.
- **A script behaves like the plugin it ports.** `ScriptStrategyTest` runs the imbalance taker written in Lua beside the
  C++ plugin over a synthetic market and asserts the same loop report, fills, PnL, execution counters and stats; on
  SPY's full day the two made the same 48,643 fills and digests. It also checks the sandbox (no `io`, `os`, `load` or
  collector), that table order, `math.random` and `table.sort` repeat across Lua states, that no address reaches a
  script, the instruction, allocation and memory budgets (which a `pcall` cannot catch), the caps on timers, orders,
  cancels and params, and every error naming its line.
- **A script's run reproduces from its own source.** `WatchTest` watches a script's Lab run to `reproduced`;
  `ServerTest` verifies one after its script was edited.
- **The book against a model.** `BookModelTest` applies a random operation stream to the list, to the ladder at two
  steps and to a plain `std::map` model, and compares the books after every operation. `MarketBookTest` runs 10,000
  seeded random events, valid and invalid, checking every invariant after each.
- **The fill rules by hand.** `ExecutionSimulatorTest.WorkedExample1SweepsThenExpiresAndRepeatsIdentically`,
  `RestingOrderTest.WorkedExample2QueuePosition`, `RestingOrderTest.WorkedExample3CrossingAddFillsAtOurPrice` and
  `EventLoopTest.TieGoesToMarketInTheSpecWalkthrough` pin the fill rules and the loop order with worked examples.
- **Determinism.** `BacktestDeterminismTest.*`: two backtests are identical and leave the history digest of a plain
  replay. The server's `CommandLogTest`, `LabTest` and `WatchTest` reproduce sessions and runs from their logs and saved
  specs to the same digests.
- **Strategies on synthetic days.** `OrbTest` covers every rule of the ORB: entries both ways, partial and zero fills,
  the exact range boundaries, the stop, the flatten timer, a halt, missing trades, a carried position.
- **The real day.** The full replay of 2019-12-30, every symbol: 264,469,515 events, 0 rejections, every book empty at
  the end, invariants checked every 1,024 events (258,271 checks), the same digest in every configuration.
- **Across platforms.** The Docker image (Linux, g++ 14) and the macOS build (Apple clang 17) give the same digests on
  the same file: `7f1e922654415cf3` for AAPL on the slice and `b00db289f7c388b2` for all its 6,529,870 events.

### Real-data tests

```bash
CHRONOS_ITCH_FILE=data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin ./build/simulator_tests --gtest_filter='ItchRealDataTest.*:BacktestDeterminismTest.*'
```

`CHRONOS_ITCH_SYMBOL` defaults to `AAPL` and `CHRONOS_ITCH_DATE` to `2019-12-30`. Without `CHRONOS_ITCH_FILE` they skip.
Use the slice: these tests read the file several times.

## Sanitizers

`CHRONOS_SANITIZE` is `OFF` (the default), `ON` (= `address,undefined`), or any `-fsanitize=` list. Every sanitized
build adds `-fno-sanitize-recover=all`, so any report aborts and a passing test is a clean one.

```bash
cmake -S . -B build/ubsan -DCHRONOS_SANITIZE=undefined && cmake --build build/ubsan -j && ctest --test-dir build/ubsan
```

On macOS 27, AddressSanitizer and ThreadSanitizer hang at process startup, so they run on Linux, in CI: ASan with UBSan
under g++, and TSan under clang over the server's tests and the replay's SPSC ring and two-thread pipeline.

## CI

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on every push and pull request:

| Job | Runs |
|---|---|
| `linux-gcc-release`, `linux-clang-release`, `macos-release` | build with `-Wall -Wextra -Wpedantic` and run `ctest`, benchmarks built |
| `linux-gcc-debug-asan-ubsan` | Debug under AddressSanitizer and UBSan |
| `linux-clang-debug-ubsan`, `macos-debug-ubsan` | Debug under UBSan |
| `format` | `scripts/check-format.sh`: every C++ file against `.clang-format`, clang-format 20.1.8 |
| `isolation` | `scripts/check-isolation.sh` (the engine includes no server or web code, and links only the standard library) and `scripts/check-hot-path.sh` (no allocator or growing container on the replay path) |
| `server-tsan` | ThreadSanitizer over `chronos_server_tests` and the replay's ring and pipeline tests |
| `web` | typecheck, Vitest, build, then Playwright against a Release `chronos-serve` |

Locally the build has 0 warnings with `-Wall -Wextra -Wpedantic` under Apple clang 17 (RelWithDebInfo and Debug) and
g++ 14 (Release).

## Formatting

```bash
pipx install clang-format==20.1.8     # the version CI pins; others format differently
scripts/check-format.sh               # every tracked or new *.cpp/*.hpp; changes nothing
clang-format -i FILE                  # to rewrite one
```

The web code uses Prettier with `--single-quote --print-width 120`.
