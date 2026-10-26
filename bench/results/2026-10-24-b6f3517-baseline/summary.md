# 2026-10-24-b6f3517-baseline

Build b6f3517fcc7f, Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-24T09:13:09Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,469,515 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 132.91 | 123.14 | 499,963 | 1,989,793 | 502.6 | 463 | 703 | 1,087 | 2,943 | 43,910,763 | 0.166 | 1,814 | 593 |
| 2 | 132.49 | 122.72 | 499,968 | 1,996,174 | 501.0 | 463 | 703 | 1,151 | 3,583 | 43,910,763 | 0.166 | 1,711 | 593 |
| 3 | 128.57 | 120.16 | 499,963 | 2,057,054 | 486.1 | 463 | 671 | 991 | 2,687 | 43,910,763 | 0.166 | 1,913 | 593 |
| **median** | 132.49 | 122.72 | 499,963 | 1,996,174 | 501.0 | 463 | 703 | 1,087 | 2,943 | 43,910,763 | 0.166 | 1,814 | 593 |
| min-max | 128.57-132.91 | 120.16-123.14 | 499,963-499,968 | 1,989,793-2,057,054 | 486.1-502.6 | 463-463 | 671-703 | 991-1,151 | 2,687-3,583 | 43,910,763-43,910,763 | 0.166-0.166 | 1,711-1,913 | 593-593 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

6,529,870 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.89 | 1.70 | 12,720 | 3,462,047 | 288.9 | 175 | 575 | 1,151 | 1,855 | 1,847,460 | 0.283 | 777 | 508 |
| 2 | 1.70 | 1.69 | 0 | 3,850,836 | 259.7 | 159 | 543 | 863 | 1,279 | 1,847,460 | 0.283 | 777 | 508 |
| 3 | 1.58 | 1.58 | 0 | 4,132,804 | 242.0 | 151 | 479 | 735 | 1,087 | 1,847,460 | 0.283 | 777 | 508 |
| 4 | 1.66 | 1.66 | 0 | 3,923,577 | 254.9 | 159 | 511 | 799 | 1,151 | 1,847,460 | 0.283 | 777 | 508 |
| 5 | 1.61 | 1.61 | 0 | 4,046,724 | 247.1 | 159 | 479 | 735 | 1,023 | 1,847,460 | 0.283 | 777 | 508 |
| **median** | 1.66 | 1.66 | 0 | 3,923,577 | 254.9 | 159 | 511 | 799 | 1,151 | 1,847,460 | 0.283 | 777 | 508 |
| min-max | 1.58-1.89 | 1.58-1.70 | 0-12,720 | 3,462,047-4,132,804 | 242.0-288.9 | 151-175 | 479-575 | 735-1,151 | 1,023-1,855 | 1,847,460-1,847,460 | 0.283-0.283 | 777-777 | 508-508 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/depth:1000 | 62.8 (2 %) |
| BM_Add/depth:1000000 | 150.1 (4 %) |
| BM_Delete/depth:1000 | 29.6 (0 %) |
| BM_Delete/depth:1000000 | 50.3 (2 %) |
| BM_ExecuteFull/depth:1000 | 33.8 (1 %) |
| BM_ExecuteFull/depth:1000000 | 56.4 (4 %) |
| BM_ExecutePartial/depth:1000 | 14.4 (0 %) |
| BM_ExecutePartial/depth:1000000 | 119.8 (2 %) |
| BM_Cancel/depth:1000 | 15.1 (2 %) |
| BM_Cancel/depth:1000000 | 117.4 (0 %) |
| BM_Replace/depth:1000 | 93.4 (1 %) |
| BM_Replace/depth:1000000 | 506.0 (2 %) |
| BM_ItchSourceNext/all:1 | 53.4 (0 %) |
| BM_ItchSourceNext/all:0 | 15.0 (0 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 6.2 (8 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 64.8 (10 %) |
| BM_EventLoop/mm:0/iterations:5 | 144.0 (5 %) |
| BM_EventLoop/mm:1/iterations:5 | 244.1 (1 %) |

## Profile

```
114,709 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   21.8%    24,998  simulator::OrderBook::unlink(simulator::Order&)
   18.9%    21,714  simulator::OrderBook::add(simulator::Order&)
   14.4%    16,575  simulator::OrderBook::find(unsigned long long)
   10.3%    11,830  simulator::ItchSource::next(simulator::MarketEvent&)
    3.8%     4,380  simulator::OrderIndex::erase(unsigned long long)
    3.7%     4,262  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    3.0%     3,482  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    2.9%     3,274  simulator::ByteReader::read_u32_be(unsigned int&)
    2.7%     3,150  simulator::ByteReader::read_u64_be(unsigned long long&)
    2.7%     3,067  simulator::ByteReader::read_u16_be(unsigned short&)
    2.2%     2,518  simulator::ByteReader::read_u48_be(unsigned long long&)
    2.1%     2,436  [libsystem_malloc.dylib]
    1.7%     1,938  simulator::MarketBook::apply(simulator::MarketEvent const&)
    1.7%     1,925  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    1.0%     1,164  std::__tree_remove[abi:ne190102]<std::__tree_node_base<void*>*>(std::__tree_node_base<void*>*, std::__tree_node_base<void*>*)

Total (self + callees), share of replay time:
  100.0%   114,709  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   55.0%    63,064  simulator::MarketBook::apply(simulator::MarketEvent const&)
   27.7%    31,759  simulator::ItchSource::next(simulator::MarketEvent&)
   27.1%    31,100  simulator::OrderBook::erase(unsigned long long)
   24.4%    27,936  simulator::OrderBook::unlink(simulator::Order&)
   22.6%    25,970  simulator::OrderBook::add(simulator::Order&)
   14.4%    16,575  simulator::OrderBook::find(unsigned long long)
   11.8%    13,566  simulator::MarketBook::replace(simulator::MarketEvent const&)
    9.8%    11,188  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    9.7%    11,081  simulator::ItchDecoder::decode_add_order(simulator::ItchMessage const&, simulator::ItchAddOrder&)
    3.8%     4,380  simulator::OrderIndex::erase(unsigned long long)
    3.3%     3,841  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    2.9%     3,284  simulator::ItchDecoder::decode_order_delete(simulator::ItchMessage const&, simulator::ItchOrderDelete&)
    2.9%     3,274  simulator::ByteReader::read_u32_be(unsigned int&)
    2.7%     3,150  simulator::ByteReader::read_u64_be(unsigned long long&)
```

