# 2026-10-24-b6f3517-dirty-ladder-1

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:23:51Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 178.94 | 167.24 | 499,964 | 1,477,968 | 676.6 | 639 | 959 | 1,599 | 4,351 | 0 | 0.000 | 0 | 1,059 | 2,391 | 758 |
| 2 | 177.72 | 169.03 | 499,963 | 1,488,152 | 672.0 | 639 | 959 | 1,471 | 3,199 | 0 | 0.000 | 0 | 1,059 | 2,394 | 758 |
| 3 | 91.72 | 83.35 | 499,963 | 2,883,377 | 346.8 | 271 | 607 | 1,023 | 2,815 | 0 | 0.000 | 0 | 1,059 | 2,915 | 758 |
| **median** | 177.72 | 167.24 | 499,963 | 1,488,152 | 672.0 | 639 | 959 | 1,471 | 3,199 | 0 | 0.000 | 0 | 1,059 | 2,394 | 758 |
| min-max | 91.72-178.94 | 83.35-169.03 | 499,963-499,964 | 1,477,968-2,883,377 | 346.8-676.6 | 271-639 | 607-959 | 1,023-1,599 | 2,815-4,351 | 0-0 | 0.000-0.000 | 0-0 | 1,059-1,059 | 2,391-2,915 | 758-758 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book ladder --threads 1; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.07 | 1.04 | 0 | 6,074,423 | 164.6 | 115 | 271 | 415 | 2,175 | 0 | 0.000 | 0 | 1,059 | 901 | 738 |
| 2 | 1.25 | 1.03 | 13,116 | 5,202,152 | 192.2 | 123 | 319 | 863 | 2,175 | 0 | 0.000 | 0 | 1,059 | 735 | 738 |
| **median** | 1.17 | 1.03 | 6,558 | 5,638,288 | 178.4 | 119 | 295 | 639 | 2,175 | 0 | 0.000 | 0 | 1,059 | 818 | 738 |
| min-max | 1.07-1.25 | 1.03-1.04 | 0-13,116 | 5,202,152-6,074,423 | 164.6-192.2 | 115-123 | 271-319 | 415-863 | 2,175-2,175 | 0-0 | 0.000-0.000 | 0-0 | 1,059-1,059 | 735-901 | 738-738 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

