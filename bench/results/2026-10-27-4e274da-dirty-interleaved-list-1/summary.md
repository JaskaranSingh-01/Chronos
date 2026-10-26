# 2026-10-27-4e274da-dirty-interleaved-list-1

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T18:58:12Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 200.91 | 187.70 | 499,965 | 1,316,374 | 759.7 | 703 | 1,151 | 1,855 | 4,863 | 0 | 0.000 | 0 | 925 | 1,774 | 740 |
| 2 | 196.51 | 184.20 | 499,964 | 1,345,815 | 743.0 | 703 | 1,087 | 1,791 | 4,863 | 0 | 0.000 | 0 | 925 | 1,767 | 740 |
| 3 | 196.63 | 182.89 | 499,965 | 1,345,026 | 743.5 | 671 | 1,087 | 1,919 | 5,375 | 0 | 0.000 | 0 | 925 | 1,622 | 740 |
| **median** | 196.63 | 184.20 | 499,965 | 1,345,026 | 743.5 | 703 | 1,087 | 1,855 | 4,863 | 0 | 0.000 | 0 | 925 | 1,767 | 740 |
| min-max | 196.51-200.91 | 182.89-187.70 | 499,964-499,965 | 1,316,374-1,345,815 | 743.0-759.7 | 671-703 | 1,087-1,151 | 1,791-1,919 | 4,863-5,375 | 0-0 | 0.000-0.000 | 0-0 | 925-925 | 1,622-1,774 | 740-740 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

