# 2026-10-27-4e274da-dirty-interleaved-ladder-2

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T19:05:45Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 104.75 | 188.25 | 499,966 | 2,524,662 | 396.1 | 351 | 607 | 1,215 | 2,559 | 0 | 0.000 | 0 | 1,063 | 1,815 | 763 |
| 2 | 104.13 | 189.03 | 499,968 | 2,539,724 | 393.7 | 351 | 607 | 1,151 | 2,559 | 0 | 0.000 | 0 | 1,063 | 1,693 | 763 |
| 3 | 102.23 | 185.86 | 499,965 | 2,587,087 | 386.5 | 351 | 575 | 1,087 | 2,303 | 0 | 0.000 | 0 | 1,063 | 1,793 | 763 |
| **median** | 104.13 | 188.25 | 499,966 | 2,539,724 | 393.7 | 351 | 607 | 1,151 | 2,559 | 0 | 0.000 | 0 | 1,063 | 1,793 | 763 |
| min-max | 102.23-104.75 | 185.86-189.03 | 499,965-499,968 | 2,524,662-2,587,087 | 386.5-396.1 | 351-351 | 575-607 | 1,087-1,215 | 2,303-2,559 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 1,693-1,815 | 763-763 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

