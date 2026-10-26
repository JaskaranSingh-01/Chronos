# 2026-10-27-4e274da-dirty-quiet-ladder-1

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:28:44Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 90.67 | 78.82 | 499,972 | 2,916,927 | 342.8 | 303 | 495 | 991 | 3,327 | 0 | 0.000 | 0 | 1,060 | 1,809 | 759 |
| 2 | 78.11 | 68.80 | 499,963 | 3,385,625 | 295.4 | 255 | 431 | 703 | 3,199 | 0 | 0.000 | 0 | 1,060 | 2,079 | 759 |
| 3 | 76.82 | 68.01 | 500,134 | 3,442,524 | 290.5 | 255 | 431 | 671 | 2,943 | 0 | 0.000 | 0 | 1,060 | 1,904 | 759 |
| **median** | 78.11 | 68.80 | 499,972 | 3,385,625 | 295.4 | 255 | 431 | 703 | 3,199 | 0 | 0.000 | 0 | 1,060 | 1,904 | 759 |
| min-max | 76.82-90.67 | 68.01-78.82 | 499,963-500,134 | 2,916,927-3,442,524 | 290.5-342.8 | 255-303 | 431-495 | 671-991 | 2,943-3,327 | 0-0 | 0.000-0.000 | 0-0 | 1,060-1,060 | 1,809-2,079 | 759-759 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

