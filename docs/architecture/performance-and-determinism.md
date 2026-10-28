# Performance and determinism

Speed is a requirement, and so is the evidence for it. This document gives the rules for claiming performance, the
recorded baseline with its context, and what the digests do and do not prove. The full numbers, every configuration
and the reproduction commands are in [performance.md](../performance.md); every run's record is in
[`bench/RESULTS.md`](../../bench/RESULTS.md), with the raw JSON in `bench/results/`.

## Rules for a performance claim

1. **Profile before optimising.** A change targets what the profile shows is expensive. The profile of the default
   configuration says where the next gain is (placing new price levels, 40 % of the book thread's time).
2. **Correctness first, then speed.** A candidate passes an exact correctness gate (the same digests, 0 rejections,
   invariants passing, 0 allocations) before its speed is compared. Speed never buys a changed result
   ([I14](README.md#the-invariants)).
3. **Interleave, repeat, take medians.** Configurations run in rotating order on the same machine and input, each run
   started only when the machine is quiet, at least three full-day runs each, compared by median against the spread.
   A single run is not evidence.
4. **Name the context.** A number is quoted with its configuration, input, machine, build and method
   ([I12](README.md#the-invariants)).
5. **A faster structure is a hypothesis.** "This uses a faster data structure" is not a result. The claim is made by a
   measurement on the real workload, and kept or rejected on it. One candidate (M2.03) was rejected before it was
   built, on profile evidence; the ladder became the default only after its slots were widened to a cent, when it
   measured fastest.
6. **Know the machine.** On the measurement machine (Apple M4, four performance and six efficiency cores), a
   one-thread run that lands on an efficiency core runs about half as fast, and one matrix, most likely run in Low
   Power Mode, took about 2.4 times the CPU time and was discarded. One matrix was discarded for exactly
   that reason. Results from different sessions are compared only if they were interleaved.

## What is measured

| Measure | Meaning | Caveat |
|---|---|---|
| **Throughput** | market events applied per wall second over the full day, every symbol | includes the wait for the file: the 8.25 GB day does not stay in the 16 GiB machine's page cache, about 500,000 major faults a run |
| **CPU time** | user plus system time, all threads | two threads use more CPU for less wall time |
| **Mean cost per event** | wall time over events, on the book thread | |
| **Cost distribution** | p50, p90, p99, p99.9 of the mean cost of 1,024-event batches | not a per-event latency: the clock (41.67 ns ticks) cannot time one event ([Q8](README.md#open-questions-and-known-gaps)) |
| **Allocations** | every allocation in the process from just before the replay to just after, all threads, counted by a replacing `operator new` | executables only; the library never links it |
| **Heap change** | blocks and bytes in use before and after the replay | on Linux, blocks are not counted ([Q9](README.md#open-questions-and-known-gaps)) |
| **Memory** | peak footprint (the process's own memory) and the bytes reserved before the replay | peak RSS also counts the mapped input |

### Throughput and latency are different goals

A second thread that decodes while the first applies raised throughput by 44 % on the ladder (wall 78.1 s to 54.1 s)
while spending 37 % more CPU (68.8 s to 94.3 s). That is a throughput optimisation: it overlaps work, partly with the
wait for the disk. A latency optimisation would lower the cost of a single event on one core: fewer cache misses when
a new level is placed, cheaper decoding. Chronos has done mostly the first; the second is the next target, and would
need a finer clock to measure ([Q8](README.md#open-questions-and-known-gaps)).

## The recorded baseline

| | |
|---|---|
| **Workload** | NASDAQ TotalView-ITCH 5.0, 2019-12-30: 8,251,407,909 bytes, 268,744,780 messages, 264,469,515 market events, 8,906 symbols, every symbol replayed (`--all`) |
| **Machine** | Apple M4 (4 performance + 6 efficiency cores), 16 GiB, macOS (Darwin 27.0.0), on AC power, Low Power Mode off |
| **Build** | Apple clang 17.0.0, CMake `Release` (`-O3 -DNDEBUG`); engine at `4e274da` plus the cent-wide ladder step (committed afterwards) |
| **Method** | median of 3 interleaved full-day runs per configuration, rotating order, each started at a load average below 4; the previous engine (`b6f3517`) in every round |
| **Evidence** | `bench/results/2026-10-27-4e274da-dirty-quiet-*/` and its matrix; summarised in [`bench/RESULTS.md`](../../bench/RESULTS.md) |

| | previous engine `b6f3517` | ladder, one thread | **ladder, two threads (default)** |
|---|---|---|---|
| events/s (median) | 3,999,129 | 3,385,625 | **4,885,679** |
| events/s (min-max) | 3,784,162-4,009,271 | 2,916,927-3,442,524 | 4,715,459-4,970,876 |
| wall s / CPU s | 66.1 / 57.1 | 78.1 / 68.8 | 54.1 / 94.3 |
| ns/event, mean | 250.1 | 295.4 | 204.7 |
| batch-mean p50 / p99 / p99.9 ns | 215 / 607 / 2,431 | 255 / 703 / 3,199 | 191 / 447 / 863 |
| allocations during the replay | 43,910,763 | 0 | 0 |
| peak footprint / reserved MB | 593 / – | 759 / 1,060 | 763 / 1,063 |
| digest, rejections | `6897f945bcef75c2`, 0 | the same | the same |

Read with the table: the default is 22.2 % faster than the previous engine in throughput, for 65 % more CPU. On one
thread the allocation-free engine is 15.3 % slower than the previous one: the pools and indices that remove 43.9
million allocations cost single-thread time, and the second thread more than pays for them. The list backend's
numbers and the slice runs are in [performance.md](../performance.md).

Only this machine has been measured. The same digests were produced on Linux (g++ in Docker) for a 256 MiB slice, but
no Linux or x86 throughput is recorded.

## Allocations

The replay reserves everything before it starts: the order pool, the level pool, the order and level indices
(open addressing), the ladder windows and the pipeline's ring, all sized from the run's capacity. A full structure stops
the run with its name; nothing grows ([I10](README.md#the-invariants)). The full day peaks at 1,924,078 live orders
against a default capacity of 4,194,304.

The evidence is threefold: the counting allocator (0 over the full day, both threads); the heap's blocks and bytes,
unchanged across the replay (on macOS; on Linux only the bytes, [Q9](README.md#open-questions-and-known-gaps)); and a
check in CI that no allocator call or growing container call appears on the replay path outside its setup.

## Data structures and their trade-offs

| Choice | Why | Cost | Alternative |
|---|---|---|---|
| Fixed pools with free lists for orders and levels | no allocation; stable addresses; cache-friendly reuse | capacity is a run parameter | growing containers (the previous engine: 43.9 million allocations a day) |
| Open-addressing indices keyed by (book, id) and (book, side, price) | one probe sequence in a flat table; shared by all books | a full table stops the run | node-based maps |
| Levels as a doubly linked list, best first (`list`) | finding an existing level is one hash lookup | placing a new level walks to its neighbours | a sorted array per side (not evaluated) |
| A window of 256 cent-wide slots around each side's best over the list (`ladder`, default) | a new level near the best is linked after the nearest set slot: a short scan | the scan, then the list walk off the grid, is still 40 % of the book thread | a bitmap of the set slots, so the scan reads a few words (proposed) |
| A single-producer single-consumer ring between decoder and book threads | the decoder runs alongside the book; no lock; each index on its own cache line, with a cached copy of the other's | a second core | one thread (still available: `--threads 1`) |

The pools and indices, the ladder and the two-thread pipeline were each measured on the full day against what they
replaced before becoming defaults; the alternatives marked not evaluated or proposed were not.

## Determinism

**The contract:** the same input bytes, configuration, build and commands produce the same books, fills, reports and
PnL, byte for byte, on every run ([I5](README.md#the-invariants)). It holds because:

- no result reads a wall clock: all time in a result is exchange time from events (the replay loop reads a clock only
  to sample the cost per event, when asked);
- all ordering is explicit: (exchange time, class, sequence), market first at ties;
- prices, quantities and money are integers; no floating point enters a result;
- no thread decides an order: the two-thread pipeline only decodes ahead, and the book thread applies in source
  order;
- the interactive server applies commands at recorded boundaries, so wall-clock timing never reaches the engine.

Documented exceptions: none in the engine. In the server, wall-clock timing decides which snapshots are published and
which are dropped, which affects only what a client sees, never a digest.

### Digests

| Digest | Covers |
|---|---|
| **Book digest** | one symbol's book: each live order's side, price, id, remaining quantity and sequence, side by side, best level first, front of the queue first, mixed byte by byte in a fixed byte order, so it does not depend on the host |
| **Combined replay digest** | (symbol id, last sequence, book digest) over every book, in symbol-id order |
| **Execution digest** | the simulator's counters and the resting shadow orders |
| **Portfolio digest** | every fill booked, in order, then position, cash, PnL, fees, drawdown and open orders |

Sessions and Lab runs record the book, execution and portfolio digests; the CLI's backtest prints the book digest
([event-and-execution-model.md](event-and-execution-model.md#the-portfolio)).

### What the digests prove

| Check | Proves | Does not prove |
|---|---|---|
| The combined digest equal across list and ladder, one thread and two, and the previous engine | the configurations and the optimisations changed no result | that the result is right: with every book empty at the close, it fingerprints each symbol's adds and replaces ([Q2](README.md#open-questions-and-known-gaps)) |
| A backtest's book digest equal to a plain replay's | simulation did not change history ([I3](README.md#the-invariants)) | |
| The same digests from Linux/g++ and macOS/clang (AAPL and every symbol on the 256 MiB slice, whose books are still live at its end) | the result does not depend on the compiler, library or host | |
| A Lab run verified, or watched, to the same execution and portfolio digests | the run reproduces fill by fill on this build and input | anything, if the build differs: the verdict is then "different code", not a pass |
| A session reproduced from its command log | an interactive session is as reproducible as a backtest | |

Correctness of the reconstruction itself rests on the 0 rejections, the invariants and the tests against a reference
model, not on the digest. A rolling digest of every event's resulting top of book, or a per-message comparison with an
independent source, is the recorded way to make the digest carry correctness too.

### Regression testing

Any engine change re-runs the correctness gate on the full day (when the file is present) and the whole test suite;
the digests are the regression oracle. A change that is meant to alter results records the new digests and why.

## Headless isolation

The server and the UI must not slow or change the headless engine ([I8](README.md#the-invariants)):

| Criterion | Status |
|---|---|
| The engine library and headless executables include and link no server, network, JSON or UI code | **Verified** by CI on every push |
| The headless digests are unchanged by the UI work | **Verified**: the full-day replay digest has stayed `6897f945bcef75c2`; the step-wise event loop the server needed left backtest outputs identical (commit `05623c2`) |
| Headless performance is within run-to-run spread after the UI work | **Partial**: the event-loop benchmark stayed within its before spread when the loop became step-wise (`05623c2`); the headless replay does not run through the event loop, so its throughput was untouched by construction. No full-day before-and-after matrix of the server's addition is recorded ([Q7](README.md#open-questions-and-known-gaps)) |
| A numeric tolerance such as 1 % | **Not adopted**: the criterion is the measured spread, which on this machine is wider than 1 % |
| Observability does not cost the headless path | per-event timing is compiled into a separate loop used only when latency sampling is asked for, so the default loop reads no clock |

## The UI's own measurement

A browser test measures where the page's time goes while it watches a market maker's run at Max: about 14,000
WebSocket messages a second, 60 frames a second, no frame over 50 ms, the socket handler at 1.8 % of the main thread
([ui.md](../ui.md#measuring-the-page)). That measures the page; it says nothing about the engine.
