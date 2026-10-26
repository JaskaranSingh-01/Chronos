# 2026-10-27-4e274da-dirty-quiet-ladder-2

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:33:06Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 54.13 | 94.30 | 499,966 | 4,885,679 | 204.7 | 191 | 287 | 447 | 831 | 0 | 0.000 | 0 | 1,063 | 1,872 | 763 |
| 2 | 56.09 | 98.25 | 499,965 | 4,715,459 | 212.1 | 199 | 287 | 463 | 1,023 | 0 | 0.000 | 0 | 1,063 | 2,125 | 763 |
| 3 | 53.20 | 92.85 | 499,963 | 4,970,876 | 201.2 | 191 | 271 | 431 | 863 | 0 | 0.000 | 0 | 1,063 | 2,057 | 763 |
| **median** | 54.13 | 94.30 | 499,965 | 4,885,679 | 204.7 | 191 | 287 | 447 | 863 | 0 | 0.000 | 0 | 1,063 | 2,057 | 763 |
| min-max | 53.20-56.09 | 92.85-98.25 | 499,963-499,966 | 4,715,459-4,970,876 | 201.2-212.1 | 191-199 | 271-287 | 431-463 | 831-1,023 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 1,872-2,125 | 763-763 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

