# 2026-10-24-b6f3517-dirty-list-2

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:17:19Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 101.24 | 189.95 | 499,965 | 2,612,411 | 382.8 | 351 | 575 | 927 | 1,535 | 0 | 0.000 | 0 | 929 | 2,985 | 744 |
| 2 | 105.09 | 196.75 | 499,963 | 2,516,563 | 397.4 | 367 | 575 | 959 | 1,663 | 0 | 0.000 | 0 | 929 | 2,442 | 744 |
| 3 | 101.26 | 190.09 | 499,975 | 2,611,822 | 382.9 | 351 | 543 | 895 | 1,471 | 0 | 0.000 | 0 | 929 | 2,903 | 744 |
| **median** | 101.26 | 190.09 | 499,965 | 2,611,822 | 382.9 | 351 | 575 | 927 | 1,535 | 0 | 0.000 | 0 | 929 | 2,903 | 744 |
| min-max | 101.24-105.09 | 189.95-196.75 | 499,963-499,975 | 2,516,563-2,612,411 | 382.8-397.4 | 351-367 | 543-575 | 895-959 | 1,471-1,663 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 2,442-2,985 | 744-744 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book list --threads 2; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.15 | 2.25 | 0 | 5,692,525 | 175.7 | 111 | 335 | 479 | 799 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| 2 | 1.26 | 2.23 | 12,722 | 5,181,412 | 193.0 | 115 | 399 | 671 | 1,407 | 0 | 0.000 | 0 | 929 | 920 | 723 |
| **median** | 1.20 | 2.24 | 6,361 | 5,436,968 | 184.3 | 113 | 367 | 575 | 1,103 | 0 | 0.000 | 0 | 929 | 956 | 723 |
| min-max | 1.15-1.26 | 2.23-2.25 | 0-12,722 | 5,181,412-5,692,525 | 175.7-193.0 | 111-115 | 335-399 | 479-671 | 799-1,407 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 920-992 | 723-723 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

