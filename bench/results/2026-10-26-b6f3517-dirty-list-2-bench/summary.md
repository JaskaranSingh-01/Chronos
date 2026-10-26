# 2026-10-26-b6f3517-dirty-list-2-bench

Build b6f3517fcc7f (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-26T11:30:55Z.

## Full day, --all

no runs

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

--book list --threads 2; 6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | heap change bytes | reserved MB | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0.87 | 1.41 | 12,722 | 7,518,165 | 133.0 | 71 | 287 | 495 | 1,279 | 0 | 0.000 | 0 | 929 | 988 | 723 |
| 2 | 0.78 | 1.53 | 0 | 8,331,454 | 120.0 | 75 | 231 | 351 | 511 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| 3 | 0.73 | 1.37 | 0 | 8,963,819 | 111.6 | 67 | 223 | 415 | 671 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| 4 | 0.65 | 1.28 | 0 | 10,038,503 | 99.6 | 63 | 191 | 287 | 367 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| 5 | 0.66 | 1.31 | 0 | 9,871,604 | 101.3 | 63 | 191 | 303 | 575 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| **median** | 0.73 | 1.37 | 0 | 8,963,819 | 111.6 | 67 | 223 | 351 | 575 | 0 | 0.000 | 0 | 929 | 992 | 723 |
| min-max | 0.65-0.87 | 1.28-1.53 | 0-12,722 | 7,518,165-10,038,503 | 99.6-133.0 | 63-75 | 191-287 | 287-495 | 367-1,279 | 0-0 | 0.000-0.000 | 0-0 | 929-929 | 988-992 | 723-723 |

ns/event p50-p99.9 on the book thread: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/book:0/depth:1000 | 21.7 (1 %) |
| BM_Add/book:1/depth:1000 | 22.1 (1 %) |
| BM_Add/book:0/depth:1000000 | 59.6 (5 %) |
| BM_Add/book:1/depth:1000000 | 65.2 (10 %) |
| BM_Delete/book:0/depth:1000 | 15.6 (1 %) |
| BM_Delete/book:1/depth:1000 | 15.8 (1 %) |
| BM_Delete/book:0/depth:1000000 | 27.2 (2 %) |
| BM_Delete/book:1/depth:1000000 | 27.2 (5 %) |
| BM_ExecuteFull/book:0/depth:1000 | 18.5 (1 %) |
| BM_ExecuteFull/book:1/depth:1000 | 18.2 (1 %) |
| BM_ExecuteFull/book:0/depth:1000000 | 30.3 (1 %) |
| BM_ExecuteFull/book:1/depth:1000000 | 30.1 (3 %) |
| BM_ExecutePartial/book:0/depth:1000 | 7.9 (0 %) |
| BM_ExecutePartial/book:1/depth:1000 | 7.9 (3 %) |
| BM_ExecutePartial/book:0/depth:1000000 | 60.5 (1 %) |
| BM_ExecutePartial/book:1/depth:1000000 | 61.1 (4 %) |
| BM_Cancel/book:0/depth:1000 | 8.0 (2 %) |
| BM_Cancel/book:1/depth:1000 | 7.9 (0 %) |
| BM_Cancel/book:0/depth:1000000 | 59.8 (1 %) |
| BM_Cancel/book:1/depth:1000000 | 61.2 (5 %) |
| BM_Replace/book:0/depth:1000 | 36.7 (1 %) |
| BM_Replace/book:1/depth:1000 | 36.9 (1 %) |
| BM_Replace/book:0/depth:1000000 | 223.1 (1 %) |
| BM_Replace/book:1/depth:1000000 | 237.6 (3 %) |
| BM_Churn/book:0 | 61.3 (23 %) |
| BM_Churn/book:1 | 29.6 (35 %) |
| BM_DispatchSelected/book:0 | 24.8 (0 %) |
| BM_DispatchSelected/book:1 | 42.5 (23 %) |
| BM_DispatchFixed/book:0 | 25.8 (31 %) |
| BM_DispatchFixed/book:1 | 24.1 (1 %) |
| BM_ItchSourceNext/all:1 | 27.7 (1 %) |
| BM_ItchSourceNext/all:0 | 7.6 (1 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 2.9 (1 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 34.8 (4 %) |
| BM_Ring | 6.3 (2 %) |
| BM_EventLoop/mm:0/iterations:5 | 67.2 (7 %) |
| BM_EventLoop/mm:1/iterations:5 | 96.2 (8 %) |

## Profile

```
49,386 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   43.9%    21,662  simulator::BasicOrderBook<(simulator::Dispatch)0>::new_level(simulator::Side, simulator::Price)
   15.1%     7,471  simulator::BasicOrderBook<(simulator::Dispatch)0>::remove(simulator::Order&)
    8.9%     4,406  simulator::BasicOrderBook<(simulator::Dispatch)0>::find(unsigned long long)
    8.5%     4,221  simulator::MarketBook::apply(simulator::MarketEvent const&)
    8.3%     4,108  simulator::BasicOrderBook<(simulator::Dispatch)0>::add(simulator::Order&)
    2.8%     1,389  simulator::PipelineSource::next(simulator::MarketEvent&)
    2.7%     1,321  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    1.6%       802  simulator::LevelIndex::erase(unsigned int, simulator::Side, simulator::Price)
    0.6%       316  simulator::OrderPool::release(simulator::Order&)
    0.6%       285  simulator::PriceLevel::add(simulator::Order&)
    0.6%       282  simulator::BasicOrderBook<(simulator::Dispatch)0>::room(simulator::Side, simulator::Price, simulator::Order const*) const
    0.5%       264  simulator::MarketBook::add(simulator::MarketEvent const&)
    0.5%       252  simulator::Order::id() const
    0.5%       229  simulator::PriceLevel::remove(simulator::Order&)
    0.4%       214  simulator::Order::side() const

Total (self + callees), share of replay time:
  100.0%    49,386  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   54.8%    27,086  simulator::BasicOrderBook<(simulator::Dispatch)0>::add(simulator::Order&)
   44.2%    21,814  simulator::BasicOrderBook<(simulator::Dispatch)0>::new_level(simulator::Side, simulator::Price)
   42.2%    20,840  simulator::MarketBook::replace(simulator::MarketEvent const&)
   24.0%    11,841  simulator::MarketBook::apply(simulator::MarketEvent const&)
   23.2%    11,475  simulator::MarketBook::add(simulator::MarketEvent const&)
   17.6%     8,678  simulator::BasicOrderBook<(simulator::Dispatch)0>::remove(simulator::Order&)
    8.9%     4,406  simulator::BasicOrderBook<(simulator::Dispatch)0>::find(unsigned long long)
    3.0%     1,457  simulator::PipelineSource::next(simulator::MarketEvent&)
    2.1%     1,028  simulator::BasicOrderBook<(simulator::Dispatch)0>::drop_level(simulator::Side, simulator::PriceLevel&)
    1.6%       802  simulator::LevelIndex::erase(unsigned int, simulator::Side, simulator::Price)
    1.2%       595  simulator::MarketBook::reduce(simulator::MarketEvent const&)
    0.7%       343  simulator::PriceLevel::add(simulator::Order&)
    0.6%       316  simulator::OrderPool::release(simulator::Order&)
    0.6%       297  simulator::OrderPool::acquire(unsigned long long, simulator::Side, simulator::Price, simulator::Quantity, long long)
```

