# 2026-10-27-4e274da-dirty-quiet-baseline

Build unknown, Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:24:54Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 65.96 | 56.54 | 499,963 | 4,009,271 | 249.4 | 215 | 383 | 607 | 2,815 | 43,910,763 | 0.166 | - | - | 1,812 | 593 |
| 2 | 69.89 | 60.67 | 499,965 | 3,784,162 | 264.3 | 231 | 399 | 639 | 2,431 | 43,910,763 | 0.166 | - | - | 1,787 | 593 |
| 3 | 66.13 | 57.07 | 499,966 | 3,999,129 | 250.1 | 215 | 383 | 575 | 2,431 | 43,910,763 | 0.166 | - | - | 2,392 | 593 |
| **median** | 66.13 | 57.07 | 499,965 | 3,999,129 | 250.1 | 215 | 383 | 607 | 2,431 | 43,910,763 | 0.166 | - | - | 1,812 | 593 |
| min-max | 65.96-69.89 | 56.54-60.67 | 499,963-499,966 | 3,784,162-4,009,271 | 249.4-264.3 | 215-231 | 383-399 | 575-639 | 2,431-2,815 | 43,910,763-43,910,763 | 0.166-0.166 | - | - | 1,787-2,392 | 593-593 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

