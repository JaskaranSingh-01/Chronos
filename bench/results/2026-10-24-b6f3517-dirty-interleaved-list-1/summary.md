# 2026-10-24-b6f3517-dirty-interleaved-list-1

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:39:17Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 76.87 | 68.65 | 499,970 | 3,440,288 | 290.7 | 255 | 431 | 671 | 2,431 | 0 | 0.000 | 0 | 925 | 2,427 | 740 |
| 2 | 76.81 | 68.50 | 499,964 | 3,443,032 | 290.4 | 255 | 431 | 639 | 2,559 | 0 | 0.000 | 0 | 925 | 2,635 | 740 |
| 3 | 86.15 | 76.12 | 499,964 | 3,070,008 | 325.7 | 287 | 479 | 895 | 2,815 | 0 | 0.000 | 0 | 925 | 1,937 | 740 |
| **median** | 76.87 | 68.65 | 499,964 | 3,440,288 | 290.7 | 255 | 431 | 671 | 2,559 | 0 | 0.000 | 0 | 925 | 2,427 | 740 |
| min-max | 76.81-86.15 | 68.50-76.12 | 499,964-499,970 | 3,070,008-3,443,032 | 290.4-325.7 | 255-287 | 431-479 | 639-895 | 2,431-2,815 | 0-0 | 0.000-0.000 | 0-0 | 925-925 | 1,937-2,635 | 740-740 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

