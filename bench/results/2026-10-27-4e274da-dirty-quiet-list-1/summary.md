# 2026-10-27-4e274da-dirty-quiet-list-1

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:26:16Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 80.99 | 71.71 | 499,965 | 3,265,535 | 306.2 | 271 | 447 | 735 | 2,943 | 0 | 0.000 | 0 | 925 | 1,865 | 740 |
| 2 | 82.04 | 73.01 | 499,964 | 3,223,637 | 310.2 | 271 | 447 | 735 | 2,559 | 0 | 0.000 | 0 | 925 | 1,884 | 740 |
| 3 | 79.66 | 71.05 | 499,979 | 3,320,002 | 301.2 | 271 | 431 | 671 | 2,943 | 0 | 0.000 | 0 | 925 | 1,987 | 740 |
| **median** | 80.99 | 71.71 | 499,965 | 3,265,535 | 306.2 | 271 | 447 | 735 | 2,943 | 0 | 0.000 | 0 | 925 | 1,884 | 740 |
| min-max | 79.66-82.04 | 71.05-73.01 | 499,964-499,979 | 3,223,637-3,320,002 | 301.2-310.2 | 271-271 | 431-447 | 671-735 | 2,559-2,943 | 0-0 | 0.000-0.000 | 0-0 | 925-925 | 1,865-1,987 | 740-740 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

