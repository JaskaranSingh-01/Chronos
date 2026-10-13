# Benchmark results: baseline, M2.12, M2.04

Measured on 2026-10-13. Every number comes from the JSON in `bench/results/`; `bench/summary.py <folder>...`
prints the tables. Re-run from the repo root with:

```
bench/run.sh <label>      # ~17 min at the baseline, ~13 min now
```

| Stage | Change | Results folder |
|---|---|---|
| baseline | none: commit `a5ed53b` plus the harness | `results/2026-10-13-a5ed53b-dirty-baseline` |
| M2.12 | `OrderBook::unlink` uses `order.level_`; `std::map` only to erase an emptied level | `results/2026-10-13-a5ed53b-dirty-m2.12` |
| M2.03 | rejected without a run (see Decisions) | – |
| M2.04 | `OrderIndex`: open-addressing `OrderId → Order*` replaces `std::unordered_map` | `results/2026-10-13-a5ed53b-dirty-m2.04` |

M2.04 is measured on top of M2.12. All three ran on `a5ed53b` with the uncommitted changes, so every JSON says
`"dirty": true`; the label tells the stages apart.

## Machine and toolchain

| | |
|---|---|
| CPU | Apple M4: 4 performance + 6 efficiency cores, 16 MiB L2 on the performance cluster |
| Memory | 16 GiB, 16 KiB pages |
| OS | macOS 27.0.1 (Darwin 27.0.0) |
| Compiler | Apple clang 17.0.0 (clang-1700.0.13.5), C++20, libc++ |
| Build | CMake 4.3.2, `Release` (`-O3 -DNDEBUG`) |
| Libraries | Google Benchmark v1.9.1, GoogleTest v1.16.0 (FetchContent, release tags) |
| Timer | `std::chrono::steady_clock`, a 24 MHz counter: one tick is 41.67 ns |
| Profiler | `/usr/bin/sample` at 1 ms (`xctrace` needs a full Xcode, which is not installed) |
| Other load | a corporate laptop: an endpoint-security agent and a JVM used about 0.4 of a core throughout |

**Why Release.** It is the configuration spec §8 names for benchmarks. The profile runs the same binary that is
timed: `sample` takes function names from the symbol table, so a RelWithDebInfo build (`-O2 -g`, different
code) is not needed.

**Inputs** (`data/itch`, not committed):
- Full day: `12302019.NASDAQ_ITCH50.bin`, 8,251,407,909 bytes, fingerprint `6e463072d3ccbf76`.
  - 268,744,780 messages and 264,460,539 events.
  - Add 118.6 M, Delete 114.4 M, Replace 21.6 M, Execute 5.8 M, Cancel 2.8 M, Trade 1.2 M.
  - The peak is 1,924,078 live orders over 8,906 books. AAPL peaks at 27,110 and MSFT at 23,005.
- Slice: `12302019.NASDAQ_ITCH50.first-256MiB.bin`, 268,435,456 bytes, fingerprint `812aade968a7dc04`.
  - 7,844,756 messages and 6,520,959 events, from 04:00 to 09:30:10, so pre-market only.
  - It ends cut mid-message, with 6 books crossed after the open. Every slice run therefore exits 1 by design;
    its digest `b00db289f7c388b2` is still deterministic.

## Protocol (`bench/run.sh`)

1. Build `build/bench`: Release, `-DCHRONOS_BENCH=ON`.
2. Replay the full day, `--all`, 3 times with `--latency --json`. Run 1 is labelled cold and runs 2-3 warm. Each
   JSON's `major_faults` shows what really came from disk.
3. Replay the slice, `--all --allow-truncated-tail`, 5 times.
4. Run `simulator_bench --benchmark_repetitions=5`.
5. Sample one more full-day `--all` replay with `sample`. `bench/summary.py --profile` keeps only the samples
   inside `MarketReplay::run`.
6. Write the JSON, logs, `profile-top.txt` and `summary.md` (medians, min-max) to `bench/results/<date>-<commit>[-dirty][-label]/`.

What the replay JSON fields mean:

| Metric | Definition |
|---|---|
| wall | `steady_clock` around `MarketReplay::run()`. It excludes opening and mapping the file, building the pool and the end-of-run checks. |
| CPU | `getrusage` user + sys over the same span |
| events/s, ns/event | events ÷ wall, wall ÷ events |
| ns/event p50-p99.9 | With `--latency`, the loop reads the clock every 1,024 events and records that batch's mean ns/event in `LatencyHistogram` (log-linear buckets, at most 6.25 % high). The percentiles are of the 258,262 batch means of the full day (6,368 for the slice), not of single events. |
| allocations | calls to the global `operator new` during `run()` (`app/alloc_counter.cpp`, linked into the executable only) |
| peak RSS | `getrusage` `ru_maxrss` (bytes on macOS) for the whole process. It includes resident pages of the mapped input. |
| peak footprint | `proc_pid_rusage` `ri_lifetime_max_phys_footprint`: the process's own memory, without clean file pages |
| major faults | `ru_majflt` during `run()`: page faults that waited for the disk |

## Full day, `--all`

Median of 3 runs, with min-max in brackets. Every run of every stage gave digest `6897f945bcef75c2`, 0
rejections and all 8,906 books empty at 20:00.

| | baseline | M2.12 | M2.04 |
|---|---|---|---|
| wall s | 222.2 (213.5-224.0) | 195.7 (195.1-196.5) | 131.3 (130.9-134.7) |
| CPU s (user + sys) | 210.9 (203.6-211.1) | 185.8 (185.8-186.4) | 122.2 (121.4-124.4) |
| events/s | 1,190,206 | 1,351,231 | 2,013,783 |
| ns/event, mean | 840.2 | 740.1 | 496.6 |
| ns/event p50 | 799 | 735 | 463 |
| ns/event p90 | 1,151 | 959 | 671 |
| ns/event p99 | 1,727 | 1,471 | 1,087 |
| ns/event p99.9 | 4,607 | 3,583 | 3,199 |
| allocations (per event) | 184,193,726 (0.697) | 184,193,726 (0.697) | 43,910,762 (0.166) |
| peak RSS MB | 1,921 | 1,885 | 1,950 |
| peak footprint MB | 571 | 571 | 593 |
| major faults | 499,964 | 499,964 | 499,963 |

- **M2.12:** CPU −11.9 % and wall −11.9 %.
- **M2.04:** CPU −34.2 % against M2.12.
- **Baseline to M2.04:** CPU −42.0 %, wall −40.9 % and events/s +69 %.
- Each step is far outside the run-to-run range. The widest range is the baseline's, at about 3.5 % of CPU.

## Slice, `--all`

Median of 5 runs.

| | baseline | M2.12 | M2.04 |
|---|---|---|---|
| wall s | 2.23 | 2.13 | 1.65 |
| CPU s | 2.22 | 2.13 | 1.65 |
| events/s | 2,929,197 | 3,054,495 | 3,942,475 |
| ns/event mean / p50 / p99 / p99.9 | 341 / 215 / 1,087 / 1,407 | 327 / 199 / 1,151 / 1,599 | 254 / 159 / 799 / 1,151 |
| allocations (per event) | 5,588,314 (0.857) | 5,588,314 (0.857) | 1,847,459 (0.283) |
| peak footprint MB | 497 | 497 | 508 |

## Micro-benchmarks (`simulator_bench`)

Median of 5 repetitions, in ns per operation (per message for `ItchSource`). The coefficient of variation is in
brackets.

- **Book benchmarks.** `MarketBook::apply` on one book pre-filled to the given depth.
  - ITCH-like ids rise with gaps. Prices are a geometric number of ticks off the mid.
  - Delete and ExecuteFull remove orders added just before, which are still in cache.
  - ExecutePartial, Cancel and Replace hit random resting orders.
- **`ItchSource` benchmarks.** `ItchSource::next` over 2^20 messages built with `ItchBuilder` in the slice's
  message mix.
- **Pool benchmark.** Release plus acquire on a half-full `OrderPool`.

| Benchmark | baseline | M2.12 | M2.04 |
|---|---|---|---|
| Add, depth 1,000 | 95.4 (2 %) | 93.5 (1 %) | 62.7 (3 %) |
| Add, depth 1,000,000 | 256.8 (9 %) | 271.1 (5 %) | 143.3 (4 %) |
| Delete, depth 1,000 | 81.2 (1 %) | 45.9 (3 %) | 29.9 (2 %) |
| Delete, depth 1,000,000 | 80.7 (3 %) | 45.1 (10 %) | 51.2 (1 %) |
| ExecuteFull, depth 1,000 | 82.0 (1 %) | 48.2 (1 %) | 34.4 (8 %) |
| ExecuteFull, depth 1,000,000 | 81.6 (2 %) | 48.7 (3 %) | 56.5 (5 %) |
| ExecutePartial, depth 1,000 | 22.0 (1 %) | 20.8 (1 %) | 14.3 (1 %) |
| ExecutePartial, depth 1,000,000 | 202.7 (13 %) | 235.2 (20 %) | 91.7 (27 %) |
| Cancel, depth 1,000 | 22.3 (2 %) | 22.0 (2 %) | 14.9 (2 %) |
| Cancel, depth 1,000,000 | 201.5 (19 %) | 225.9 (10 %) | 104.4 (19 %) |
| Replace, depth 1,000 | 170.9 (0 %) | 137.0 (1 %) | 95.7 (1 %) |
| Replace, depth 1,000,000 | 1,041.9 (6 %) | 1,096.9 (5 %) | 487.0 (12 %) |
| ItchSource, all symbols | 52.1 (0 %) | 52.7 (1 %) | 52.7 (1 %) |
| ItchSource, one symbol of 100 | 14.5 (0 %) | 14.6 (1 %) | 14.5 (0 %) |
| Pool, capacity 1,024 | 5.5 (10 %) | 5.7 (0 %) | 6.2 (8 %) |
| Pool, capacity 4,194,304 | 58.7 (16 %) | 74.7 (20 %) | 80.2 (5 %) |

- **1M-depth Delete and ExecuteFull.** After M2.04 they are slower: 45.1 → 51.2 ns and 48.7 → 56.5 ns.
  - At that depth the table is 48 % full (2^21 slots for 1,000,512 orders). `erase()` scans the rest of the probe
    run, while `unordered_map` unlinked a node that was still in cache.
  - The difference is close to the drift seen on unchanged code: the pool benchmark moved from 58.7 to 74.7 to
    80.2 ns across the three runs.
- **The full day.** There, most removals hit cold orders, and the M2.04 replay is 34 % faster.

## Profiles: top 5

Self time (top of stack) as a share of the samples inside `MarketReplay::run`, from each stage's
`profile-top.txt`. Release inlines the hot containers: `std::map::find` and `erase` sit inside `OrderBook::unlink`
and `add`, and `OrderIndex::find` sits inside `OrderBook::find`.

| # | baseline (176,396 samples) | M2.12 (162,299) | M2.04 (108,959) |
|---|---|---|---|
| 1 | `OrderBook::unlink` 42.8 % | `OrderBook::unlink` 20.4 % | `OrderBook::unlink` 21.7 % |
| 2 | `OrderBook::find` 10.9 % | `OrderBook::add` 13.8 % | `OrderBook::add` 18.6 % |
| 3 | `OrderBook::add` 9.7 % | `OrderBook::find` 11.6 % | `OrderBook::find` 14.7 % |
| 4 | malloc/free 7.1 % | malloc/free 10.2 % | `ItchSource::next` 10.2 % |
| 5 | `ItchSource::next` 5.0 % | `unordered_map::remove` 9.7 % | `ItchDecoder::read_add_order` 3.8 % |

After M2.04, counting callees:
- `ItchSource::next` (framing + decode) is 28.1 %.
- malloc/free has fallen to 2.0 % self.
- `OrderIndex::erase` is 3.7 %.

## Decisions

### M2.12: landed
- **Evidence.** The baseline profile had `OrderBook::unlink` at 42.8 % self, which is the `std::map` level search.
- **Result.** CPU went from 210.9 to 185.8 s (−11.9 %), with the digest unchanged. The test suite (436 tests at
  that stage) passes unchanged.
- **Why the gain is smaller than 42.8 %.** The loop is bound by memory latency.
  - Removing the tree walk took 42 k samples out of `unlink`. Misses that the out-of-order core had overlapped with
    the walk then became visible elsewhere.
  - `unordered_map::remove` rose from 3.9 k to 15.7 k samples, and `ItchSource::next` from 8.8 k to 12.6 k.
  - So a self-time share is an upper bound on the saving, not a forecast.

### M2.03: rejected, not run
- **Rehashing is negligible.** `__do_rehash` was 0.10 % of replay samples in the baseline and 0.09 % after M2.12.
  The 10 % gate is far off.
- **Allocations are nodes.** In the baseline, 140,270,523 of the 184,193,726 allocations are index nodes, one per
  Add or Replace. Almost all of the other 43.9 M are `std::map` level nodes.
- **No capacity to reserve.** In `--all` mode the 8,906 books share one pool, so no book has a capacity of its own
  to reserve from `--capacity`.
- **Where pre-sizing went.** It is part of M2.04: a book that owns its pool sizes its `OrderIndex` from the pool's
  capacity, and that index never grows.

### M2.04: landed
- **Evidence.** After M2.12, the index cost 30.2 % of the samples: `OrderBook::find`, `unordered_map` insert and
  remove, and node malloc/free. That count leaves out the duplicate-id check inlined in `add`. The gate is 10 %.
- **Result.** CPU went from 185.8 to 122.2 s (−34.2 %), and allocations fell from 0.697 to 0.166 per event. The
  digest is unchanged.
- **Cost.** Peak footprint grew by 22 MB (571 → 593 MB), because the tables run at most half full with 16 B a slot,
  where `unordered_map` used 32 B nodes for live orders only.
- **Design.**
  - Each `OrderBook` keeps its own index. The `MarketEvent` contract only promises ids unique within a symbol, so
    one shared table would need (symbol, id) keys.
  - Probing is linear, with backward-shift erase (no tombstones) and Fibonacci hashing.
  - A book that owns its pool gets the pool's capacity up front and never grows.
  - A book on the shared pool starts at 16 slots and doubles, a few times per book over the day. It never shrinks;
    a `ponytail:` comment marks this.
- **Tests.** `OrderIndexTest` covers:
  - colliding ids;
  - wrap-around at the end of the table;
  - backward shift, checked against `std::unordered_map` over 200,000 random steps;
  - a capacity from the pool that is never outgrown;
  - growth.

  The `OrderBook`, `MarketBook` and real-data suites pass unchanged. Mutating the backward-shift condition or the
  growth check makes these tests fail.

### Steady-state allocations (M2.09)
Not zero: 0.166 per event, 43.9 M over the day.
- **What they are.** Almost all are `std::map` nodes. A price level allocates one when it opens and frees it when it
  empties; about 31 % of the 140.3 M inserts open a new level.
- **The rest.** Per-book `OrderIndex` growth and book creation account for the remainder.
- **The zero-allocation test.** M2.09 was not asserted. It applies only once M2.05 replaces the map.

### M2.11
`sizeof(Order)` is already 80 bytes at `a5ed53b`, because `side_` and `state_` already sit together. The reorder has
nothing left to recover. `sizeof(MarketEvent)` is 48 and `sizeof(PriceLevel)` is 40.

## What the evidence suggests next (not done in this round)

1. **M2.05, price-indexed levels.** The `std::map` is now the largest book cost, so its 10 % gate is met. M2.05
   would also remove the last per-event allocations.
   - `OrderBook::unlink` (21.7 % self) is mostly the erase walk for the ~44 M levels that empty during the day, plus
     the first touch of the level.
   - `OrderBook::add` (18.6 % self) includes the `try_emplace` walk.
   - Node malloc/free adds about 2 %.
2. **Source decode (M2.06, M2.07).** `ItchSource::next` is 28.1 % with its callees.
   - About 11 % of the loop is in four small out-of-line `ByteReader` readers (`read_u16/u32/u48/u64_be`,
     2.3-3.0 % each). Decoder helpers such as `read_add_order` (3.8 %) add more.
   - The virtual `next()` call itself does not show: `MarketReplay::run` self is 2.8 %.
   - Batching reads (M2.06) would not remove the per-field calls; making `ByteReader` inline is the direct test.
   - M2.07 is gated on a Google Benchmark gain; `BM_ItchSourceNext` (52.7 ns per message) is the benchmark to watch.
3. **M2.08, the filtered fast path.** Not for `--all`: no order message is filtered there, and only 4,275,335
   messages (1.6 %) have unused types.
   - It matters for `--symbol` runs: framing alone is 14.5 ns per message, about 3.9 s of a full-file single-symbol
     run.
   - Measure a `--symbol` run before deciding.
4. **Index lookups.** `OrderBook::find` is still 14.7 %, mostly the first probe of each event, which misses the
   cache.
   - `MarketBook` looks each id up twice: once itself, then again in `OrderBook`.
   - The second lookup now hits the cache, so passing `Order*` through would probably save little.

## Caveats

- **Page cache.** The 8.25 GB file does not stay cached on this 16 GiB machine.
  - Every full-day run had about 500 k major faults, which is the whole file read from disk again. That includes the
    "warm" runs 2-3, so all full-day runs are effectively cold.
  - Wall minus CPU is 9-13 s per run (I/O wait and fault handling). CPU time is the stable metric.
  - `sudo purge` was not used.
  - The slice stays cached. Only the first slice run after the full-day runs faults (~12.7 k major faults, about
    200 MB).
- **Timer resolution.** One `steady_clock` tick is 41.67 ns, so single events cannot be timed.
  - The latency percentiles are of 1,024-event batch means. A 50 µs stall raises its batch by only ~49 ns/event.
  - Each value is a bucket upper bound, at most 6.25 % high.
  - The slice's p99.9 is the 7th-worst of 6,368 batches, so it is noisy.
- **Cost of `--latency`.** Over 6 interleaved slice runs each, the median CPU was 1.63 s without it and 1.64 s with
  it. That is within the run-to-run range of 1.61-1.72 s. The untimed loop is a separate instantiation with no clock
  reads.
- **Profiles.** `sample` attributes a cache miss to whichever instruction waits for it, and inlining merges callees
  into callers. Shares show where the time is, not exactly what a change will save (see M2.12).
- **Micro-benchmarks.** Each benchmarks one book on its own.
  - At depth 1,000 the book stays in cache, and the coefficient of variation is mostly 0-3 % (8 % at most).
  - At depth 1,000,000 the benchmarks are DRAM-bound and drift 10-25 % between runs.
  - macOS cannot pin a thread to a performance core.
- **Peak RSS.** It counts resident pages of the mapped input, so it follows the page cache rather than the code.
  Compare peak footprint instead.
- **Background load.** The machine was in normal use: an endpoint-security agent and a JVM were running.
