# 2026-10-24-b6f3517-dirty-interleaved-ladder-1

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:41:31Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 79.79 | 71.59 | 499,963 | 3,314,434 | 301.7 | 271 | 447 | 671 | 2,303 | 0 | 0.000 | 0 | 1,059 | 2,478 | 758 |
| 2 | 79.39 | 70.80 | 499,963 | 3,331,257 | 300.2 | 271 | 431 | 671 | 2,815 | 0 | 0.000 | 0 | 1,059 | 2,338 | 758 |
| 3 | 81.13 | 72.08 | 500,031 | 3,259,757 | 306.8 | 271 | 447 | 735 | 2,815 | 0 | 0.000 | 0 | 1,059 | 2,013 | 758 |
| **median** | 79.79 | 71.59 | 499,963 | 3,314,434 | 301.7 | 271 | 447 | 671 | 2,815 | 0 | 0.000 | 0 | 1,059 | 2,338 | 758 |
| min-max | 79.39-81.13 | 70.80-72.08 | 499,963-500,031 | 3,259,757-3,331,257 | 300.2-306.8 | 271-271 | 431-447 | 671-735 | 2,303-2,815 | 0-0 | 0.000-0.000 | 0-0 | 1,059-1,059 | 2,013-2,478 | 758-758 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

