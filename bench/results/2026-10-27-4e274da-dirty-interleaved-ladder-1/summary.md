# 2026-10-27-4e274da-dirty-interleaved-ladder-1

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T19:03:50Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 1; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 192.22 | 177.24 | 499,966 | 1,375,891 | 726.8 | 671 | 1,087 | 1,919 | 5,631 | 0 | 0.000 | 0 | 1,060 | 1,704 | 759 |
| 2 | 214.43 | 191.49 | 499,965 | 1,233,386 | 810.8 | 703 | 1,215 | 2,559 | 8,703 | 0 | 0.000 | 0 | 1,060 | 1,538 | 759 |
| 3 | 198.91 | 182.58 | 499,964 | 1,329,563 | 752.1 | 671 | 1,151 | 2,047 | 5,631 | 0 | 0.000 | 0 | 1,060 | 1,595 | 759 |
| **median** | 198.91 | 182.58 | 499,965 | 1,329,563 | 752.1 | 671 | 1,151 | 2,047 | 5,631 | 0 | 0.000 | 0 | 1,060 | 1,595 | 759 |
| min-max | 192.22-214.43 | 177.24-191.49 | 499,964-499,966 | 1,233,386-1,375,891 | 726.8-810.8 | 671-703 | 1,087-1,215 | 1,919-2,559 | 5,631-8,703 | 0-0 | 0.000-0.000 | 0-0 | 1,060-1,060 | 1,538-1,704 | 759-759 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

