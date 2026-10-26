# 2026-10-27-4e274da-dirty-interleaved-list-2

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T19:00:11Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 118.63 | 218.02 | 499,966 | 2,229,343 | 448.6 | 383 | 735 | 1,215 | 2,431 | 0 | 0.000 | 0 | 929 | 1,752 | 744 |
| 2 | 134.11 | 225.59 | 499,968 | 1,971,977 | 507.1 | 399 | 799 | 1,791 | 6,911 | 0 | 0.000 | 0 | 929 | 1,806 | 744 |
| 3 | 112.90 | 207.39 | 499,967 | 2,342,601 | 426.9 | 367 | 671 | 1,151 | 2,175 | 0 | 0.000 | 0 | 929 | 1,717 | 744 |
| **median** | 118.63 | 218.02 | 499,967 | 2,229,343 | 448.6 | 383 | 735 | 1,215 | 2,431 | 0 | 0.000 | 0 | 929 | 1,752 | 744 |
| min-max | 112.90-134.11 | 207.39-225.59 | 499,966-499,968 | 1,971,977-2,342,601 | 426.9-507.1 | 367-399 | 671-799 | 1,151-1,791 | 2,175-6,911 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 1,717-1,806 | 744-744 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

