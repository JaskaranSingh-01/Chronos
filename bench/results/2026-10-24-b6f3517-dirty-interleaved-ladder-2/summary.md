# 2026-10-24-b6f3517-dirty-interleaved-ladder-2

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T17:42:33Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

--book ladder --threads 2; 264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 61.94 | 110.91 | 499,965 | 4,270,030 | 234.2 | 223 | 319 | 495 | 799 | 0 | 0.000 | 0 | 1,063 | 2,369 | 762 |
| 2 | 55.50 | 99.19 | 499,973 | 4,765,287 | 209.8 | 199 | 287 | 431 | 735 | 0 | 0.000 | 0 | 1,063 | 2,631 | 762 |
| 3 | 71.53 | 118.46 | 499,966 | 3,697,267 | 270.5 | 231 | 383 | 831 | 2,687 | 0 | 0.000 | 0 | 1,063 | 1,993 | 762 |
| **median** | 61.94 | 110.91 | 499,966 | 4,270,030 | 234.2 | 223 | 319 | 495 | 799 | 0 | 0.000 | 0 | 1,063 | 2,369 | 762 |
| min-max | 55.50-71.53 | 99.19-118.46 | 499,965-499,973 | 3,697,267-4,765,287 | 209.8-270.5 | 199-231 | 287-383 | 431-831 | 735-2,687 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 1,993-2,631 | 762-762 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all

no runs

