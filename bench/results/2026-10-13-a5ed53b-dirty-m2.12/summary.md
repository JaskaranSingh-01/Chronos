# 2026-10-13-a5ed53b-dirty-m2.12

Build a5ed53bf194e (dirty), Release, Apple LLVM 17.0.0 (clang-1700.0.13.5); Apple M4, 10 cores, 16 GiB, Darwin 27.0.0; 2026-10-12T22:06:29Z.

## Full day, --all: 12302019.NASDAQ_ITCH50.bin, 8,251,407,909 bytes, fingerprint 6e463072d3ccbf76

264,460,539 events, 0 rejected, digest 6897f945bcef75c2 (the same in every run), 8,906 books, 0 not empty at the end, peak live orders 1,924,078; every run succeeded

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 (cold) | 195.07 | 185.76 | 499,964 | 1,355,744 | 737.6 | 735 | 959 | 1,471 | 3,199 | 184,193,726 | 0.697 | 1,989 | 571 |
| 2 | 196.54 | 186.36 | 499,964 | 1,345,590 | 743.2 | 735 | 991 | 1,535 | 3,583 | 184,193,726 | 0.697 | 1,839 | 571 |
| 3 | 195.72 | 185.77 | 499,963 | 1,351,231 | 740.1 | 735 | 959 | 1,471 | 3,711 | 184,193,726 | 0.697 | 1,885 | 571 |
| **median** | 195.72 | 185.77 | 499,964 | 1,351,231 | 740.1 | 735 | 959 | 1,471 | 3,583 | 184,193,726 | 0.697 | 1,885 | 571 |
| min-max | 195.07-196.54 | 185.76-186.36 | 499,963-499,964 | 1,345,590-1,355,744 | 737.6-743.2 | 735-735 | 959-991 | 1,471-1,535 | 3,199-3,711 | 184,193,726-184,193,726 | 0.697-0.697 | 1,839-1,989 | 571-571 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## Slice, --all: 12302019.NASDAQ_ITCH50.first-256MiB.bin, 268,435,456 bytes, fingerprint 812aade968a7dc04

6,520,959 events, 0 rejected, digest b00db289f7c388b2 (the same in every run), 8,906 books, 8,888 not empty at the end, peak live orders 1,004,059; runs 1, 2, 3, 4, 5 exited non-zero (see the logs)

| run | wall s | CPU s | major faults | events/s | ns/event | p50 | p90 | p99 | p99.9 | allocations | alloc/event | peak RSS MB | peak footprint MB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 2.55 | 2.34 | 12,720 | 2,560,419 | 390.6 | 231 | 831 | 1,471 | 2,175 | 5,588,314 | 0.857 | 749 | 497 |
| 2 | 2.20 | 2.17 | 0 | 2,962,412 | 337.6 | 199 | 703 | 1,279 | 2,047 | 5,588,314 | 0.857 | 766 | 497 |
| 3 | 2.13 | 2.11 | 0 | 3,055,915 | 327.2 | 199 | 671 | 1,151 | 1,599 | 5,588,314 | 0.857 | 766 | 497 |
| 4 | 2.12 | 2.11 | 0 | 3,076,425 | 325.1 | 199 | 703 | 1,023 | 1,471 | 5,588,314 | 0.857 | 766 | 497 |
| 5 | 2.13 | 2.13 | 0 | 3,054,495 | 327.4 | 199 | 671 | 1,151 | 1,407 | 5,588,314 | 0.857 | 766 | 497 |
| **median** | 2.13 | 2.13 | 0 | 3,054,495 | 327.4 | 199 | 703 | 1,151 | 1,599 | 5,588,314 | 0.857 | 766 | 497 |
| min-max | 2.12-2.55 | 2.11-2.34 | 0-12,720 | 2,560,419-3,076,425 | 325.1-390.6 | 199-231 | 671-831 | 1,023-1,471 | 1,407-2,175 | 5,588,314-5,588,314 | 0.857-0.857 | 749-766 | 497-497 |

ns/event p50-p99.9: percentiles of the mean ns/event of each batch of 1024 events (timer tick 41.0 ns), not of single events.

## simulator_bench: median ns per operation (per message for ItchSource)

Coefficient of variation over the repetitions in brackets.

| benchmark | ns (cv) |
|---|---|
| BM_Add/depth:1000 | 93.5 (1 %) |
| BM_Add/depth:1000000 | 271.1 (5 %) |
| BM_Delete/depth:1000 | 45.9 (3 %) |
| BM_Delete/depth:1000000 | 45.1 (10 %) |
| BM_ExecuteFull/depth:1000 | 48.2 (1 %) |
| BM_ExecuteFull/depth:1000000 | 48.7 (3 %) |
| BM_ExecutePartial/depth:1000 | 20.8 (1 %) |
| BM_ExecutePartial/depth:1000000 | 235.2 (20 %) |
| BM_Cancel/depth:1000 | 22.0 (2 %) |
| BM_Cancel/depth:1000000 | 225.9 (10 %) |
| BM_Replace/depth:1000 | 137.0 (1 %) |
| BM_Replace/depth:1000000 | 1,096.9 (5 %) |
| BM_ItchSourceNext/all:1 | 52.7 (1 %) |
| BM_ItchSourceNext/all:0 | 14.6 (1 %) |
| BM_PoolReleaseAcquire/capacity:1024 | 5.7 (0 %) |
| BM_PoolReleaseAcquire/capacity:4194304 | 74.7 (20 %) |

## Profile

```
162,299 samples (1 ms apart) inside MarketReplay::run, from profile-sample.txt

Self (top of stack), share of replay time:
   20.4%    33,114  simulator::OrderBook::unlink(simulator::Order&)
   13.8%    22,466  simulator::OrderBook::add(simulator::Order&)
   11.6%    18,852  simulator::OrderBook::find(unsigned long long)
   10.2%    16,522  [libsystem_malloc.dylib]
    9.7%    15,711  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::remove(std::__hash_const_iterator<std::__hash_node<std::__hash_value_type<unsigned long long, simulator::Order*>, void*>*>)
    7.8%    12,628  simulator::ItchSource::next(simulator::MarketEvent&)
    4.2%     6,780  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::__emplace_unique_key_args<unsigned long long, unsigned long long, simulator::Order*>(unsigned long long const&, unsigned long long&&, simulator::Order*&&)
    2.4%     3,937  simulator::ByteReader::read_u16_be(unsigned short&)
    2.3%     3,689  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    2.0%     3,307  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
    2.0%     3,239  simulator::ByteReader::read_u64_be(unsigned long long&)
    1.8%     2,897  simulator::ByteReader::read_u32_be(unsigned int&)
    1.8%     2,881  simulator::ByteReader::read_u48_be(unsigned long long&)
    1.8%     2,853  simulator::ItchMessageReader::next(simulator::ItchMessage&)
    1.4%     2,301  simulator::MarketBook::apply(simulator::MarketEvent const&)

Total (self + callees), share of replay time:
  100.0%   162,299  simulator::MarketReplay::run(simulator::MarketEventSource&, simulator::MarketBooks&, simulator::ReplayOptions const&)
   62.3%   101,117  simulator::MarketBook::apply(simulator::MarketEvent const&)
   34.6%    56,190  simulator::OrderBook::erase(unsigned long long)
   24.2%    39,229  simulator::OrderBook::add(simulator::Order&)
   22.2%    36,011  simulator::OrderBook::unlink(simulator::Order&)
   21.2%    34,374  simulator::ItchSource::next(simulator::MarketEvent&)
   11.9%    19,269  simulator::MarketBook::replace(simulator::MarketEvent const&)
   11.6%    18,852  simulator::OrderBook::find(unsigned long long)
   10.2%    16,527  [libsystem_malloc.dylib]
    9.7%    15,711  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::remove(std::__hash_const_iterator<std::__hash_node<std::__hash_value_type<unsigned long long, simulator::Order*>, void*>*>)
    8.9%    14,522  std::__hash_table<std::__hash_value_type<unsigned long long, simulator::Order*>>::__emplace_unique_key_args<unsigned long long, unsigned long long, simulator::Order*>(unsigned long long const&, unsigned long long&&, simulator::Order*&&)
    6.6%    10,740  simulator::ItchDecoder::read_add_order(simulator::ByteReader&, simulator::ItchAddOrder&)
    6.6%    10,729  simulator::ItchDecoder::decode_add_order(simulator::ItchMessage const&, simulator::ItchAddOrder&)
    5.0%     8,157  operator new(unsigned long)
    3.2%     5,255  simulator::ItchMessageReader::next(simulator::ItchMessage&)
```

