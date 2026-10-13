# 2026-10-13-a5ed53b-dirty-m2.04

Build a5ed53bf194e (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-12T22:24:35Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,460,539 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 134.73 | 124.42 | 499,963 | 1,962,938 | 509.4 | 479 | 703 | 1,151 | 3,199 | 43,910,762 | 0.166 | 1,945 | 593 |
| 2 | 131.32 | 122.20 | 499,963 | 2,013,783 | 496.6 | 463 | 671 | 1,087 | 2,815 | 43,910,762 | 0.166 | 1,950 | 593 |
| 3 | 130.92 | 121.43 | 499,966 | 2,019,975 | 495.1 | 463 | 671 | 1,087 | 3,199 | 43,910,762 | 0.166 | 2,010 | 594 |
| **median** | 131.32 | 122.20 | 499,963 | 2,013,783 | 496.6 | 463 | 671 | 1,087 | 3,199 | 43,910,762 | 0.166 | 1,950 | 593 |
| min-max | 130.92-134.73 | 121.43-124.42 | 499,963-499,966 | 1,962,938-2,019,975 | 495.1-509.4 | 463-479 | 671-703 | 1,087-1,151 | 2,815-3,199 | 43,910,762-43,910,762 | 0.166-0.166 | 1,945-2,010 | 593-594 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

6,520,959 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; runs 1, 2, 3, 4, 5 exited non-zero (see the logs)

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.15 | 1.78 | 12,732 | 3,028,413 | 330.2 | 175 | 639 | 1,727 | 6,399 | 1,847,459 | 0.283 | 584 | 508 |
| 2 | 1.65 | 1.65 | 0 | 3,942,475 | 253.7 | 159 | 511 | 799 | 1,023 | 1,847,459 | 0.283 | 777 | 508 |
| 3 | 1.73 | 1.66 | 0 | 3,760,733 | 265.9 | 159 | 543 | 799 | 3,583 | 1,847,459 | 0.283 | 735 | 508 |
| 4 | 1.61 | 1.60 | 0 | 4,054,288 | 246.7 | 159 | 479 | 671 | 959 | 1,847,459 | 0.283 | 777 | 508 |
| 5 | 1.59 | 1.59 | 0 | 4,093,462 | 244.3 | 159 | 463 | 799 | 1,151 | 1,847,459 | 0.283 | 777 | 508 |
| **median** | 1.65 | 1.65 | 0 | 3,942,475 | 253.7 | 159 | 511 | 799 | 1,151 | 1,847,459 | 0.283 | 777 | 508 |
| min-max | 1.59-2.15 | 1.59-1.78 | 0-12,732 | 3,028,413-4,093,462 | 244.3-330.2 | 159-175 | 463-639 | 671-1,727 | 959-6,399 | 1,847,459-1,847,459 | 0.283-0.283 | 584-777 | 508-508 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/depth:1000 | 62.7 (3 %) |
| BM_Add/depth:1000000 | 143.3 (4 %) |
| BM_Delete/depth:1000 | 29.9 (2 %) |
| BM_Delete/depth:1000000 | 51.2 (1 %) |
| BM_ExecuteFull/depth:1000 | 34.4 (8 %) |
| BM_ExecuteFull/depth:1000000 | 56.5 (5 %) |
| BM_ExecutePartial/depth:1000 | 14.3 (1 %) |
| BM_ExecutePartial/depth:1000000 | 91.7 (27 %) |
| BM_Cancel/depth:1000 | 14.9 (2 %) |
| BM_Cancel/depth:1000000 | 104.4 (19 %) |
| BM_Replace/depth:1000 | 95.7 (1 %) |
| BM_Replace/depth:1000000 | 487.0 (12 %) |
| BM_ItchSourceNext/all:1 | 52.7 (1 %) |
| BM_ItchSourceNext/all:0 | 14.5 (0 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 6.2 (8 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 80.2 (5 %) |

## Profile

```
108,959 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   21.7%    23,633  simulator::OrderBook::unlink(simulator::Order&)
   18.6%    20,214  simulator::OrderBook::add(simulator::Order&)
   14.7%    15,983  simulator::OrderBook::find(unsigned long long)
   10.2%    11,149  simulator::ItchSource::next(simulator::MarketEvent&)
    3.8%     4,174  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    3.7%     4,070  simulator::OrderIndex::erase(unsigned long long)
    3.0%     3,224  simulator::ByteReader::read_u64_be(unsigned long long&)
    2.9%     3,145  simulator::ByteReader::read_u16_be(unsigned short&)
    2.8%     3,028  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    2.8%     3,007  simulator::ByteReader::read_u32_be(unsigned int&)
    2.3%     2,480  simulator::ByteReader::read_u48_be(unsigned long long&)
    2.0%     2,190  [libsystem_malloc.dylib]
    1.7%     1,841  simulator::MarketBook::apply(simulator::MarketEvent const&)
    1.7%     1,824  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    1.1%     1,150  std::__tree_remove[abi:ne190102]<std::__tree_node_base<void*>*>(std::__tree_node_base<void*>*, std::__tree_node_base<void*>*)

Total (self + callees), share of replay time:
  100.0%   108,959  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   54.7%    59,654  simulator::MarketBook::apply(simulator::MarketEvent const&)
   28.1%    30,663  simulator::ItchSource::next(simulator::MarketEvent&)
   26.9%    29,299  simulator::OrderBook::erase(unsigned long long)
   24.2%    26,320  simulator::OrderBook::unlink(simulator::Order&)
   22.3%    24,292  simulator::OrderBook::add(simulator::Order&)
   14.7%    15,983  simulator::OrderBook::find(unsigned long long)
   11.9%    12,983  simulator::MarketBook::replace(simulator::MarketEvent const&)
    9.8%    10,640  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    9.6%    10,508  simulator::ItchDecoder::decode_add_order(simulator::ItchMessage const&, simulator::ItchAddOrder&)
    3.7%     4,070  simulator::OrderIndex::erase(unsigned long long)
    3.5%     3,771  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    3.1%     3,403  simulator::ItchDecoder::decode_order_delete(simulator::ItchMessage const&, simulator::ItchOrderDelete&)
    3.0%     3,224  simulator::ByteReader::read_u64_be(unsigned long long&)
    2.9%     3,145  simulator::ByteReader::read_u16_be(unsigned short&)
```

