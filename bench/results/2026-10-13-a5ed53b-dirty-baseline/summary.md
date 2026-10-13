# 2026-10-13-a5ed53b-dirty-baseline

Build a5ed53bf194e (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-12T21:49:51Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,460,539 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 222.20 | 210.86 | 499,964 | 1,190,206 | 840.2 | 831 | 1,151 | 1,727 | 4,607 | 184,193,726 | 0.697 | 1,806 | 571 |
| 2 | 224.00 | 211.10 | 499,986 | 1,180,613 | 847.0 | 799 | 1,151 | 1,855 | 4,863 | 184,193,726 | 0.697 | 1,922 | 571 |
| 3 | 213.50 | 203.64 | 499,964 | 1,238,716 | 807.3 | 799 | 1,087 | 1,599 | 3,839 | 184,193,726 | 0.697 | 1,921 | 572 |
| **median** | 222.20 | 210.86 | 499,964 | 1,190,206 | 840.2 | 799 | 1,151 | 1,727 | 4,607 | 184,193,726 | 0.697 | 1,921 | 571 |
| min-max | 213.50-224.00 | 203.64-211.10 | 499,964-499,986 | 1,180,613-1,238,716 | 807.3-847.0 | 799-831 | 1,087-1,151 | 1,599-1,855 | 3,839-4,863 | 184,193,726-184,193,726 | 0.697-0.697 | 1,806-1,922 | 571-572 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

6,520,959 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; runs 1, 2, 3, 4, 5 exited non-zero (see the logs)

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.60 | 2.41 | 12,720 | 2,504,708 | 399.2 | 223 | 863 | 1,471 | 2,303 | 5,588,314 | 0.857 | 766 | 497 |
| 2 | 2.21 | 2.21 | 0 | 2,949,155 | 339.1 | 207 | 735 | 1,087 | 1,215 | 5,588,314 | 0.857 | 766 | 497 |
| 3 | 2.25 | 2.25 | 0 | 2,895,542 | 345.4 | 215 | 703 | 1,151 | 1,407 | 5,588,314 | 0.857 | 766 | 497 |
| 4 | 2.13 | 2.12 | 0 | 3,067,089 | 326.0 | 215 | 639 | 991 | 1,215 | 5,588,314 | 0.857 | 766 | 497 |
| 5 | 2.23 | 2.22 | 0 | 2,929,197 | 341.4 | 215 | 735 | 1,087 | 1,471 | 5,588,314 | 0.857 | 766 | 497 |
| **median** | 2.23 | 2.22 | 0 | 2,929,197 | 341.4 | 215 | 735 | 1,087 | 1,407 | 5,588,314 | 0.857 | 766 | 497 |
| min-max | 2.13-2.60 | 2.12-2.41 | 0-12,720 | 2,504,708-3,067,089 | 326.0-399.2 | 207-223 | 639-863 | 991-1,471 | 1,215-2,303 | 5,588,314-5,588,314 | 0.857-0.857 | 766-766 | 497-497 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/depth:1000 | 95.4 (2 %) |
| BM_Add/depth:1000000 | 256.8 (9 %) |
| BM_Delete/depth:1000 | 81.2 (1 %) |
| BM_Delete/depth:1000000 | 80.7 (3 %) |
| BM_ExecuteFull/depth:1000 | 82.0 (1 %) |
| BM_ExecuteFull/depth:1000000 | 81.6 (2 %) |
| BM_ExecutePartial/depth:1000 | 22.0 (1 %) |
| BM_ExecutePartial/depth:1000000 | 202.7 (13 %) |
| BM_Cancel/depth:1000 | 22.3 (2 %) |
| BM_Cancel/depth:1000000 | 201.5 (19 %) |
| BM_Replace/depth:1000 | 170.9 (0 %) |
| BM_Replace/depth:1000000 | 1,041.9 (6 %) |
| BM_ItchSourceNext/all:1 | 52.1 (0 %) |
| BM_ItchSourceNext/all:0 | 14.5 (0 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 5.5 (10 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 58.7 (16 %) |

## Profile

```
176,396 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   42.8%    75,419  simulator::OrderBook::unlink(simulator::Order&)
   10.9%    19,157  simulator::OrderBook::find(unsigned long long)
    9.7%    17,041  simulator::OrderBook::add(simulator::Order&)
    7.1%    12,612  [libsystem_malloc.dylib]
    5.0%     8,827  simulator::ItchSource::next(simulator::MarketEvent&)
    4.2%     7,402  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::__emplace_unique_key_args<unsigned long long, unsigned long long, simulator::Order*>(unsigned long long const&, unsigned long long&&, simulator::Order*&&)
    2.2%     3,946  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::remove(std::__hash_const_iterator<std::__hash_node<std::__hash_value_type<unsigned long long, simulator::Order*>, void*>*>)
    2.1%     3,674  simulator::ByteReader::read_u16_be(unsigned short&)
    1.9%     3,338  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    1.8%     3,172  simulator::ByteReader::read_u64_be(unsigned long long&)
    1.6%     2,836  simulator::ByteReader::read_u32_be(unsigned int&)
    1.5%     2,709  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    1.4%     2,431  simulator::ByteReader::read_u48_be(unsigned long long&)
    1.2%     2,110  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    1.0%     1,775  simulator::MarketBook::apply(simulator::MarketEvent const&)

Total (self + callees), share of replay time:
  100.0%   176,396  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   68.6%   121,000  simulator::MarketBook::apply(simulator::MarketEvent const&)
   47.0%    82,856  simulator::OrderBook::erase(unsigned long long)
   44.4%    78,259  simulator::OrderBook::unlink(simulator::Order&)
   19.9%    35,055  simulator::OrderBook::add(simulator::Order&)
   16.0%    28,278  simulator::ItchSource::next(simulator::MarketEvent&)
   11.5%    20,362  simulator::MarketBook::replace(simulator::MarketEvent const&)
   10.9%    19,157  simulator::OrderBook::find(unsigned long long)
    9.0%    15,911  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::__emplace_unique_key_args<unsigned long long, unsigned long long, simulator::Order*>(unsigned long long const&, unsigned long long&&, simulator::Order*&&)
    7.2%    12,616  [libsystem_malloc.dylib]
    5.5%     9,631  simulator::ItchDecoder::decode_add_order(simulator::ItchMessage const&, simulator::ItchAddOrder&)
    5.4%     9,596  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    4.9%     8,688  operator new(unsigned long)
    2.6%     4,499  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    2.2%     3,946  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::remove(std::__hash_const_iterator<std::__hash_node<std::__hash_value_type<unsigned long long, simulator::Order*>, void*>*>)
```

