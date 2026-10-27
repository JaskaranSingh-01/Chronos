# Performance

Chronos processes **4,885,679** market events per second: the full NASDAQ day of 2019-12-30, every symbol
(264,469,515 events), on one machine, an Apple M4, with the default `--book ladder --threads 2`.

## The numbers

Full day, `--all`, median of 3 runs. Each round ran the four configurations and the previous engine (commit `b6f3517`)
back to back, in rotating order, so every column met the same machine. Every run gave digest `6897f945bcef75c2` and 0
rejections.

| | baseline `b6f3517` | list × 1 | list × 2 | ladder × 1 | **ladder × 2** |
|---|---|---|---|---|---|
| CPU s (user + sys, both threads) | 57.1 | 71.7 | 101.2 | 68.8 | 94.3 |
| wall s | 66.1 | 81.0 | 57.2 | 78.1 | 54.1 |
| events/s | 3,999,129 | 3,265,535 | 4,622,252 | 3,385,625 | **4,885,679** |
| ns/event, mean | 250.1 | 306.2 | 216.3 | 295.4 | 204.7 |
| ns/event p50 / p99 / p99.9 | 215 / 607 / 2,431 | 271 / 735 / 2,943 | 207 / 463 / 863 | 255 / 703 / 3,199 | 191 / 447 / 863 |
| allocations in `run()` | 43,910,763 | 0 | 0 | 0 | 0 |
| heap change | not measured | 0 | 0 | 0 | 0 |
| peak footprint MB | 593 | 740 | 744 | 759 | 763 |
| reserved MB | – | 925 | 929 | 1,060 | 1,063 |

"× 1" and "× 2" are `--threads 1` and `--threads 2`. The default, ladder × 2, is 22.2 % faster than the baseline and
5.7 % faster than list × 2. On one thread the ladder's median is 3.7 % above the list's, within the spread of its runs,
and both are slower than the baseline (ladder −15.3 %, list −18.3 %): the pools and indices that make the replay
allocation-free cost one thread, and the second thread more than pays for them.

## Where the time goes

`/usr/bin/sample`, 1 ms, ladder × 2, self time of the book thread inside `MarketReplay::run`:

| Function | Share |
|---|---|
| `new_level`: placing a new price level (the ladder's scan of its window, then the list's walk) | 40.4 % |
| `remove` | 17.2 % |
| `find` (the order index) | 9.6 % |
| `MarketBook::apply` | 9.3 % |
| `add` | 5.6 % |

Placing new levels is the next target ([order-book.md](order-book.md)), then cheaper ITCH decoding.

## How the engine gets there

- **Zero allocations in the replay.** Every pool, index, ladder window and the ring is reserved before
  `MarketReplay::run`. A counting `operator new` (`app/alloc_counter.cpp`, linked only into executables) counts every
  allocation in the process from just before the replay to just after it, both threads included: 0 in every
  configuration on the full day. The heap's blocks and bytes in use are equal before and after.
  `scripts/check-hot-path.sh`, run in CI, finds no allocator call and no growing container call on the replay path
  outside its setup.
- **Two threads.** `PipelineSource` decodes on a thread of its own into `SpscRing`, 65,536 slots, and the replay's
  thread applies what it reads. A push writes its slot and publishes it with a store-release of the write index; a pop
  load-acquires the write index before it reads the slot, then frees it with a store-release of the read index. Each
  index sits on a 128-byte line of its own with the owner's cached copy of the other's index, so a side reads the
  other's line only when its copy says full or empty. The end of the stream and errors travel outside the ring. The
  ring's stress test runs under ThreadSanitizer in CI.
- **Locate filtering.** `ItchSource` reads a message's stock-locate field before decoding it, so a single-symbol replay
  decodes only its symbol's messages.
- **The per-symbol store** ([market-data.md](market-data.md)) makes a one-symbol replay read about 1 % of the day.

## Method

- **Machine:** Apple M4 (4 performance + 6 efficiency cores), 16 GiB, macOS 27.0.1, Apple clang 17.0.0, CMake
  `Release` (`-O3 -DNDEBUG`), on AC power with Low Power Mode off.
- **Interleaved runs.** A one-thread replay that lands on an efficiency core runs about half as fast, so runs made one
  configuration after another on a busy machine are not comparable: the engine round's first, sequential matrix
  measured list × 1 at 1.48 M events/s, against 3.44 M interleaved. Every round runs every configuration in rotating
  order, each run starting once the load average is below 4.
- **Per-event figures are a batch-derived estimate.** Each sample is the mean ns/event of a batch of 1,024 events, as
  `steady_clock` ticks every 41.67 ns on this machine and cannot time a single event. They show how the cost of an
  event varies across the day, not the latency of any one event. With two threads they are the book thread's.
- **I/O.** The 8.25 GB file does not stay in the page cache of a 16 GiB machine, so every full-day run reads it from
  disk again (about 500,000 major faults). Wall time includes that wait.
- **Memory.** Peak RSS counts the resident pages of the mapped input, so the table shows the peak footprint, the
  process's own memory. The reserved bytes are set up before the replay.
- **Selection.** The default is the configuration with the highest median that passes every correctness gate; two
  threads become the default only if they beat one thread by more than the larger run-to-run range.

[`bench/RESULTS.md`](../bench/RESULTS.md) has every run's record: the commands, the min-max ranges, the correctness
runs (`--check-invariants 1024`), the micro-benchmarks, the profiles, the decisions and the caveats. Every number
comes from the JSON in [`bench/results/`](../bench/results/).

## Reproducing

```bash
bench/run.sh [label]
```

It builds `Release` with `-DCHRONOS_BENCH=ON` in `build/bench`, replays the full day 3 times and the slice 5 times, runs
`simulator_bench` 5 times, profiles one more full-day replay with `sample` (macOS), and writes everything to
`bench/results/<date>-<commit>[-dirty][-label]/`. It needs both data files and takes about 5 minutes with the
defaults. `FLAGS` passes options to every replay (`FLAGS="--book list --threads 1" bench/run.sh list-1`), and
`FULL_RUNS`, `SLICE_RUNS` and `BENCH_REPS` set the counts (0 skips that part; `PROFILE=0` skips the profile).
`bench/summary.py <folder>...` prints and compares the tables.

A single replay prints its own timing and memory; `--latency` adds the per-batch percentiles:

```bash
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30 --latency
```

## The UI

`web/e2e/lag.spec.ts` measures where the browser's time goes ([ui.md](ui.md#measuring-the-page)). Watching a market
maker's run at Max brings about 14,000 WebSocket messages a second; the page keeps 60 frames a second, none over 50 ms,
with its socket handler at 1.9 % of the main thread.
