# 2026-10-27-4e274da-dirty-ladder-2-bench

Build 4e274da5cfd7 (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T21:55:17Z.

## Full day, --all

no runs

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book ladder --threads 2; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.74 | 1.22 | 12,722 | 8,807,888 | 113.5 | 63 | 215 | 479 | 1,151 | 0 | 0.000 | 0 | 1,063 | 924 | 742 |
| 2 | 0.83 | 1.51 | 0 | 7,888,560 | 126.8 | 71 | 239 | 431 | 2,047 | 0 | 0.000 | 0 | 1,063 | 1,010 | 742 |
| 3 | 0.74 | 1.41 | 0 | 8,869,149 | 112.8 | 71 | 207 | 303 | 1,151 | 0 | 0.000 | 0 | 1,063 | 824 | 742 |
| 4 | 0.85 | 1.53 | 0 | 7,645,775 | 130.8 | 67 | 255 | 543 | 1,919 | 0 | 0.000 | 0 | 1,063 | 966 | 742 |
| 5 | 0.91 | 1.72 | 0 | 7,214,448 | 138.6 | 79 | 287 | 431 | 767 | 0 | 0.000 | 0 | 1,063 | 1,010 | 742 |
| **median** | 0.83 | 1.51 | 0 | 7,888,560 | 126.8 | 71 | 239 | 431 | 1,151 | 0 | 0.000 | 0 | 1,063 | 966 | 742 |
| min-max | 0.74-0.91 | 1.22-1.72 | 0-12,722 | 7,214,448-8,869,149 | 112.8-138.6 | 63-79 | 207-287 | 303-543 | 767-2,047 | 0-0 | 0.000-0.000 | 0-0 | 1,063-1,063 | 824-1,010 | 742-742 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/book:0/depth:1000 | 21.4 (2 %) |
| BM_Add/book:1/depth:1000 | 20.5 (1 %) |
| BM_Add/book:0/depth:1000000 | 59.2 (12 %) |
| BM_Add/book:1/depth:1000000 | 69.1 (7 %) |
| BM_Delete/book:0/depth:1000 | 15.4 (0 %) |
| BM_Delete/book:1/depth:1000 | 15.1 (0 %) |
| BM_Delete/book:0/depth:1000000 | 27.7 (1 %) |
| BM_Delete/book:1/depth:1000000 | 27.0 (5 %) |
| BM_ExecuteFull/book:0/depth:1000 | 18.2 (8 %) |
| BM_ExecuteFull/book:1/depth:1000 | 18.3 (3 %) |
| BM_ExecuteFull/book:0/depth:1000000 | 30.5 (1 %) |
| BM_ExecuteFull/book:1/depth:1000000 | 30.7 (1 %) |
| BM_ExecutePartial/book:0/depth:1000 | 7.9 (1 %) |
| BM_ExecutePartial/book:1/depth:1000 | 7.8 (1 %) |
| BM_ExecutePartial/book:0/depth:1000000 | 60.7 (1 %) |
| BM_ExecutePartial/book:1/depth:1000000 | 61.1 (0 %) |
| BM_Cancel/book:0/depth:1000 | 8.0 (0 %) |
| BM_Cancel/book:1/depth:1000 | 8.0 (1 %) |
| BM_Cancel/book:0/depth:1000000 | 60.3 (4 %) |
| BM_Cancel/book:1/depth:1000000 | 60.1 (3 %) |
| BM_Replace/book:0/depth:1000 | 37.7 (1 %) |
| BM_Replace/book:1/depth:1000 | 35.8 (1 %) |
| BM_Replace/book:0/depth:1000000 | 225.0 (3 %) |
| BM_Replace/book:1/depth:1000000 | 222.2 (6 %) |
| BM_Churn/book:0 | 26.7 (5 %) |
| BM_Churn/book:1 | 28.1 (2 %) |
| BM_DispatchSelected/book:0 | 24.6 (1 %) |
| BM_DispatchSelected/book:1 | 26.1 (0 %) |
| BM_DispatchFixed/book:0 | 23.6 (0 %) |
| BM_DispatchFixed/book:1 | 25.3 (0 %) |
| BM_ItchSourceNext/all:1 | 27.8 (1 %) |
| BM_ItchSourceNext/all:0 | 7.6 (0 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 2.9 (8 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 31.4 (1 %) |
| BM_Ring | 6.4 (2 %) |
| BM_EventLoop/mm:0/iterations:5 | 81.7 (9 %) |
| BM_EventLoop/mm:1/iterations:5 | 108.7 (3 %) |

## Profile

```
43,230 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   40.4%    17,485  simulator::BasicOrderBook<(simulator::Dispatch)0>::new_level(simulator::Side, simulator::Price)
   17.2%     7,447  simulator::BasicOrderBook<(simulator::Dispatch)0>::remove(simulator::Order&)
    9.6%     4,160  simulator::BasicOrderBook<(simulator::Dispatch)0>::find(unsigned long long)
    9.3%     4,034  simulator::MarketBook::apply(simulator::MarketEvent const&)
    5.6%     2,436  simulator::BasicOrderBook<(simulator::Dispatch)0>::add(simulator::Order&)
    2.5%     1,087  simulator::PipelineSource::next(simulator::MarketEvent&)
    2.4%     1,058  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    1.8%       768  simulator::BasicOrderBook<(simulator::Dispatch)0>::drop_level(simulator::Side, simulator::PriceLevel&)
    1.6%       691  simulator::LevelIndex::erase(unsigned int, simulator::Side, simulator::Price)
    0.9%       394  simulator::Order::price() const
    0.8%       353  simulator::MarketBook::add(simulator::MarketEvent const&)
    0.7%       305  simulator::MarketBooks::find(unsigned int)
    0.7%       298  simulator::PriceLevel::add(simulator::Order&)
    0.7%       294  simulator::OrderPool::release(simulator::Order&)
    0.6%       241  simulator::BasicOrderBook<(simulator::Dispatch)0>::room(simulator::Side, simulator::Price, simulator::Order const*) const

Total (self + callees), share of replay time:
  100.0%    43,230  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   49.6%    21,434  simulator::BasicOrderBook<(simulator::Dispatch)0>::add(simulator::Order&)
   43.3%    18,735  simulator::MarketBook::replace(simulator::MarketEvent const&)
   40.6%    17,552  simulator::BasicOrderBook<(simulator::Dispatch)0>::new_level(simulator::Side, simulator::Price)
   25.9%    11,212  simulator::MarketBook::apply(simulator::MarketEvent const&)
   20.9%     9,035  simulator::BasicOrderBook<(simulator::Dispatch)0>::remove(simulator::Order&)
   18.5%     8,017  simulator::MarketBook::add(simulator::MarketEvent const&)
    9.6%     4,160  simulator::BasicOrderBook<(simulator::Dispatch)0>::find(unsigned long long)
    3.4%     1,476  simulator::BasicOrderBook<(simulator::Dispatch)0>::drop_level(simulator::Side, simulator::PriceLevel&)
    2.6%     1,139  simulator::PipelineSource::next(simulator::MarketEvent&)
    1.6%       691  simulator::LevelIndex::erase(unsigned int, simulator::Side, simulator::Price)
    1.3%       561  simulator::MarketBook::reduce(simulator::MarketEvent const&)
    1.0%       414  simulator::PriceLevel::add(simulator::Order&)
    0.9%       394  simulator::Order::price() const
    0.7%       305  simulator::MarketBooks::find(unsigned int)
```

