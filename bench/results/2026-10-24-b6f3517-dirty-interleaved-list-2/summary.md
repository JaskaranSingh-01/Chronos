# 2026-10-24-b6f3517-dirty-interleaved-list-2

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:40:11Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 53.27 | 94.39 | 501,255 | 4,965,065 | 201.4 | 191 | 271 | 431 | 703 | 0 | 0.000 | 0 | 929 | 2,724 | 744 |
| 2 | 53.82 | 95.08 | 499,965 | 4,913,837 | 203.5 | 191 | 271 | 431 | 735 | 0 | 0.000 | 0 | 929 | 2,552 | 744 |
| 3 | 56.01 | 97.96 | 499,965 | 4,722,188 | 211.8 | 199 | 287 | 463 | 1,023 | 0 | 0.000 | 0 | 929 | 2,022 | 744 |
| **median** | 53.82 | 95.08 | 499,965 | 4,913,837 | 203.5 | 191 | 271 | 431 | 735 | 0 | 0.000 | 0 | 929 | 2,552 | 744 |
| min-max | 53.27-56.01 | 94.39-97.96 | 499,965-501,255 | 4,722,188-4,965,065 | 201.4-211.8 | 191-199 | 271-287 | 431-463 | 703-1,023 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 2,022-2,724 | 744-744 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

