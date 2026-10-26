# 2026-10-24-b6f3517-dirty-interleaved-baseline

Build unknown, Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:38:00Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 65.68 | 56.69 | 499,963 | 4,026,805 | 248.3 | 215 | 383 | 639 | 2,815 | 43,910,763 | 0.166 | - | - | 2,393 | 593 |
| 2 | 64.13 | 55.10 | 499,969 | 4,124,034 | 242.5 | 207 | 367 | 543 | 3,199 | 43,910,763 | 0.166 | - | - | 2,706 | 594 |
| 3 | 81.30 | 68.09 | 499,964 | 3,253,070 | 307.4 | 247 | 463 | 1,023 | 3,327 | 43,910,763 | 0.166 | - | - | 1,559 | 593 |
| **median** | 65.68 | 56.69 | 499,964 | 4,026,805 | 248.3 | 215 | 383 | 639 | 3,199 | 43,910,763 | 0.166 | - | - | 2,393 | 593 |
| min-max | 64.13-81.30 | 55.10-68.09 | 499,963-499,969 | 3,253,070-4,124,034 | 242.5-307.4 | 207-247 | 367-463 | 543-1,023 | 2,815-3,327 | 43,910,763-43,910,763 | 0.166-0.166 | - | - | 1,559-2,706 | 593-594 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

