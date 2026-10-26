# 2026-10-27-4e274da-dirty-quiet-list-2

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:27:13Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 57.31 | 101.25 | 499,965 | 4,614,364 | 216.7 | 207 | 287 | 463 | 991 | 0 | 0.000 | 0 | 929 | 1,912 | 744 |
| 2 | 57.22 | 101.24 | 499,969 | 4,622,252 | 216.3 | 207 | 287 | 463 | 863 | 0 | 0.000 | 0 | 929 | 1,835 | 744 |
| 3 | 55.75 | 98.86 | 499,967 | 4,743,695 | 210.8 | 199 | 287 | 431 | 767 | 0 | 0.000 | 0 | 929 | 2,502 | 744 |
| **median** | 57.22 | 101.24 | 499,967 | 4,622,252 | 216.3 | 207 | 287 | 463 | 863 | 0 | 0.000 | 0 | 929 | 1,912 | 744 |
| min-max | 55.75-57.31 | 98.86-101.25 | 499,965-499,969 | 4,614,364-4,743,695 | 210.8-216.7 | 199-207 | 287-287 | 431-463 | 767-991 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 1,835-2,502 | 744-744 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

