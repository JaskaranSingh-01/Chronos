# 2026-10-27-4e274da-dirty-interleaved-baseline

Build unknown, Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T18:54:46Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 156.27 | 144.50 | 499,963 | 1,692,432 | 590.9 | 543 | 831 | 1,407 | 4,351 | 43,910,763 | 0.166 | - | - | 1,725 | 593 |
| 2 | 153.44 | 141.43 | 499,965 | 1,723,650 | 580.2 | 543 | 831 | 1,471 | 4,351 | 43,910,763 | 0.166 | - | - | 1,570 | 593 |
| 3 | 154.60 | 141.39 | 499,966 | 1,710,662 | 584.6 | 543 | 831 | 1,471 | 5,375 | 43,910,763 | 0.166 | - | - | 1,527 | 593 |
| **median** | 154.60 | 141.43 | 499,965 | 1,710,662 | 584.6 | 543 | 831 | 1,471 | 4,351 | 43,910,763 | 0.166 | - | - | 1,570 | 593 |
| min-max | 153.44-156.27 | 141.39-144.50 | 499,963-499,966 | 1,692,432-1,723,650 | 580.2-590.9 | 543-543 | 831-831 | 1,407-1,471 | 4,351-5,375 | 43,910,763-43,910,763 | 0.166-0.166 | - | - | 1,527-1,725 | 593-593 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

