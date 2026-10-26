# 2026-10-24-b6f3517-dirty-ladder-2

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:29:22Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 55.27 | 98.81 | 499,963 | 4,785,055 | 209.0 | 199 | 287 | 431 | 639 | 0 | 0.000 | 0 | 1,063 | 3,030 | 762 |
| 2 | 54.79 | 98.01 | 499,965 | 4,826,619 | 207.2 | 199 | 271 | 415 | 671 | 0 | 0.000 | 0 | 1,063 | 3,100 | 762 |
| 3 | 60.12 | 104.49 | 500,564 | 4,399,309 | 227.3 | 215 | 319 | 495 | 863 | 0 | 0.000 | 0 | 1,063 | 2,983 | 762 |
| **median** | 55.27 | 98.81 | 499,965 | 4,785,055 | 209.0 | 199 | 287 | 431 | 671 | 0 | 0.000 | 0 | 1,063 | 3,030 | 762 |
| min-max | 54.79-60.12 | 98.01-104.49 | 499,963-500,564 | 4,399,309-4,826,619 | 207.2-227.3 | 199-215 | 271-319 | 415-495 | 639-863 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 2,983-3,100 | 762-762 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book ladder --threads 2; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.73 | 1.45 | 0 | 8,881,205 | 112.6 | 75 | 207 | 303 | 607 | 0 | 0.000 | 0 | 1,063 | 1,010 | 741 |
| 2 | 0.78 | 1.28 | 12,737 | 8,420,290 | 118.8 | 71 | 215 | 543 | 1,343 | 0 | 0.000 | 0 | 1,063 | 884 | 741 |
| **median** | 0.76 | 1.37 | 6,368 | 8,650,748 | 115.7 | 73 | 211 | 423 | 975 | 0 | 0.000 | 0 | 1,063 | 947 | 741 |
| min-max | 0.73-0.78 | 1.28-1.45 | 0-12,737 | 8,420,290-8,881,205 | 112.6-118.8 | 71-75 | 207-215 | 303-543 | 607-1,343 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 884-1,010 | 741-741 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

