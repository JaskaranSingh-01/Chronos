# 2026-10-24-b6f3517-dirty-list-1

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:09:32Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book list --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 178.94 | 170.71 | 499,963 | 1,477,944 | 676.6 | 639 | 959 | 1,471 | 2,559 | 0 | 0.000 | 0 | 925 | 2,767 | 740 |
| 2 | 178.53 | 169.92 | 499,963 | 1,481,338 | 675.1 | 639 | 959 | 1,471 | 3,071 | 0 | 0.000 | 0 | 925 | 2,950 | 740 |
| 3 | 177.21 | 168.34 | 499,963 | 1,492,446 | 670.0 | 639 | 959 | 1,471 | 3,455 | 0 | 0.000 | 0 | 925 | 2,797 | 740 |
| **median** | 178.53 | 169.92 | 499,963 | 1,481,338 | 675.1 | 639 | 959 | 1,471 | 3,071 | 0 | 0.000 | 0 | 925 | 2,797 | 740 |
| min-max | 177.21-178.94 | 168.34-170.71 | 499,963-499,963 | 1,477,944-1,492,446 | 670.0-676.6 | 639-639 | 959-959 | 1,471-1,471 | 2,559-3,455 | 0-0 | 0.000-0.000 | 0-0 | 925-925 | 2,767-2,950 | 740-740 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book list --threads 1; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.25 | 2.25 | 0 | 2,897,111 | 345.2 | 271 | 607 | 863 | 991 | 0 | 0.000 | 0 | 925 | 988 | 719 |
| 2 | 2.65 | 2.42 | 12,720 | 2,468,014 | 405.2 | 247 | 895 | 1,407 | 2,431 | 0 | 0.000 | 0 | 925 | 825 | 719 |
| **median** | 2.45 | 2.33 | 6,360 | 2,682,562 | 375.2 | 259 | 751 | 1,135 | 1,711 | 0 | 0.000 | 0 | 925 | 907 | 719 |
| min-max | 2.25-2.65 | 2.25-2.42 | 0-12,720 | 2,468,014-2,897,111 | 345.2-405.2 | 247-271 | 607-895 | 863-1,407 | 991-2,431 | 0-0 | 0.000-0.000 | 0-0 | 925-925 | 825-988 | 719-719 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

