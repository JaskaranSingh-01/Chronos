// Micro-benchmarks of the replay hot paths (cmake -DCHRONOS_BENCH=ON): MarketBook::apply
// per event type at depth 1,000 (in cache) and 1,000,000 (not), ItchSource::next, and
// OrderPool release + acquire. Time is the mean ns per operation of a manually timed
// batch; the CPU column also counts untimed setup, so it is not comparable.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "itch/itch_test_builder.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/orders/order_pool.hpp"

namespace simulator {
namespace {

using clock_type = std::chrono::steady_clock;
using T = MarketEventType;

constexpr std::size_t kBatch = 256;          // 256 x ~100 ns, far above the 41.67 ns timer tick
constexpr std::int64_t kMid = 1'000'000;     // $100.0000 at price scale 4
constexpr std::int64_t kTick = 100;          // $0.01
constexpr std::uint64_t kShares = 1'000'000; // so partial executes and cancels never empty an order

double seconds(clock_type::duration d)
{
    return std::chrono::duration<double>(d).count();
}

// A busy symbol's order flow: ids rise with gaps, as ITCH ids do with other
// symbols' orders in between; prices sit a geometric number of ticks off the
// mid (mean ~13 ticks, capped at 500), bids below and asks above.
class Flow
{
private:
    std::mt19937_64 rng_{42};
    std::geometric_distribution<std::int64_t> ticks_{0.08};
    OrderId next_id_{0};

public:
    std::uint64_t below(std::uint64_t n)
    {
        return rng_() % n;
    }

    Price price(Side side)
    {
        const std::int64_t offset = (1 + std::min<std::int64_t>(ticks_(rng_), 500)) * kTick;
        return Price{side == Side::Buy ? kMid - offset : kMid + offset};
    }

    MarketEvent add()
    {
        next_id_ += 1 + below(200);
        const Side side = below(2) != 0 ? Side::Buy : Side::Sell;
        return {
            .order_id = next_id_, .price = price(side), .quantity = Quantity{kShares}, .type = T::Add, .side = side};
    }
};

// One book pre-filled to `depth` orders, whose ids `live` keeps.
struct FilledBook
{
    Flow flow;
    MarketBook book;
    std::vector<OrderId> live;

    explicit FilledBook(std::size_t depth)
        : book{0, depth + 2 * kBatch}
    {
        live.reserve(depth);
        for (std::size_t i = 0; i < depth; ++i) {
            const MarketEvent add = flow.add();
            book.apply(add);
            live.push_back(add.order_id);
        }
    }

    // Applies `events`, timed, as one iteration of `state`; any rejection
    // fails the benchmark, since it would time a different path.
    void timed_apply(benchmark::State& state, std::span<const MarketEvent> events)
    {
        std::size_t rejected = 0;
        const clock_type::time_point start = clock_type::now();
        for (const MarketEvent& event : events) {
            rejected += book.apply(event) != ApplyResult::Applied;
        }
        const clock_type::time_point stop = clock_type::now();
        state.SetIterationTime(seconds(stop - start) / static_cast<double>(events.size()));
        if (rejected != 0) {
            state.SkipWithError("an event was rejected");
        }
    }

    // `kBatch` new orders, applied untimed; returns them.
    const std::vector<MarketEvent>& add_untimed(std::vector<MarketEvent>& adds)
    {
        for (MarketEvent& add : adds) {
            add = flow.add();
            book.apply(add);
        }
        return adds;
    }
};

MarketEvent remove(T type, const MarketEvent& add)
{
    return {.order_id = add.order_id, .quantity = type == T::Execute ? add.quantity : Quantity{0}, .type = type};
}

void BM_Add(benchmark::State& state)
{
    FilledBook b{static_cast<std::size_t>(state.range(0))};
    std::vector<MarketEvent> adds(kBatch);
    std::vector<MarketEvent> undo(kBatch);

    for (auto _ : state) {
        for (MarketEvent& add : adds) {
            add = b.flow.add();
        }
        b.timed_apply(state, adds);
        for (std::size_t i = 0; i < kBatch; ++i) {
            b.book.apply(remove(T::Delete, adds[i]));
        }
    }
}

// Delete, or Execute of the whole remaining, of the batch of orders added just
// before (untimed), in random order: their orders are still in cache.
template <T Type>
void BM_Remove(benchmark::State& state)
{
    FilledBook b{static_cast<std::size_t>(state.range(0))};
    std::vector<MarketEvent> adds(kBatch);
    std::vector<MarketEvent> removes(kBatch);

    for (auto _ : state) {
        b.add_untimed(adds);
        for (std::size_t i = 0; i < kBatch; ++i) {
            std::swap(adds[i], adds[i + b.flow.below(kBatch - i)]);
            removes[i] = remove(Type, adds[i]);
        }
        b.timed_apply(state, removes);
    }
}

// Execute or Cancel of 1 share of a random resting order, which stays: at
// depth 1,000,000 the order and its index entry are usually cache misses.
template <T Type>
void BM_Reduce(benchmark::State& state)
{
    FilledBook b{static_cast<std::size_t>(state.range(0))};
    std::vector<MarketEvent> reduces(kBatch);

    for (auto _ : state) {
        for (MarketEvent& reduce : reduces) {
            reduce = {.order_id = b.live[b.flow.below(b.live.size())], .quantity = Quantity{1}, .type = Type};
        }
        b.timed_apply(state, reduces);
    }
}

// A random resting order replaced by a new id at a new price on its side.
void BM_Replace(benchmark::State& state)
{
    FilledBook b{static_cast<std::size_t>(state.range(0))};
    std::vector<MarketEvent> replaces(kBatch);
    std::vector<Side> sides(b.live.size());
    for (std::size_t i = 0; i < b.live.size(); ++i) {
        sides[i] = b.book.find(b.live[i])->side();
    }

    for (auto _ : state) {
        for (MarketEvent& replace : replaces) {
            const std::size_t i = b.flow.below(b.live.size());
            const MarketEvent next = b.flow.add();
            replace = {.order_id = b.live[i],
                       .new_order_id = next.order_id,
                       .price = b.flow.price(sides[i]),
                       .quantity = next.quantity,
                       .type = T::Replace};
            b.live[i] = next.order_id;
        }
        b.timed_apply(state, replaces);
    }
}

constexpr std::size_t kSymbols = 100;
constexpr std::size_t kMessages = 1 << 20;

// 100 symbols' R, then kMessages order messages in the 2019-12-30 slice's mix
// (per 1,000: A 391, F 107, D 342, X 79, U 73, E 5, C 1, P 2), with an S after
// every 5th, as the slice has 1 unused message per 5 events.
const itch_test::ItchBuilder& itch_stream()
{
    static const itch_test::ItchBuilder stream = [] {
        itch_test::ItchBuilder itch;
        std::mt19937_64 rng{7};
        std::array<std::string, kSymbols> names;
        for (std::size_t s = 0; s < kSymbols; ++s) {
            char name[8];
            std::snprintf(name, sizeof name, "S%03zu", s);
            names[s] = name;
            itch.stock_directory(static_cast<std::uint16_t>(s + 1), 1, names[s]);
        }
        for (std::size_t m = 0; m < kMessages; ++m) {
            const auto locate = static_cast<std::uint16_t>(1 + rng() % kSymbols);
            const std::string& stock = names[locate - 1];
            const std::uint64_t ts = 2 + m;
            const std::uint64_t id = 1 + rng() % (m + 1);
            const char side = rng() % 2 != 0 ? 'B' : 'S';
            const auto price = static_cast<std::uint32_t>(kMid + static_cast<std::int64_t>(rng() % 100) * kTick);
            const std::uint64_t pick = rng() % 1'000;
            if (pick < 391) {
                itch.add_order(locate, ts, m + 1, side, 100, stock, price);
            } else if (pick < 498) {
                itch.add_order_mpid(locate, ts, m + 1, side, 100, stock, price, "MPID");
            } else if (pick < 840) {
                itch.erase(locate, ts, id);
            } else if (pick < 919) {
                itch.cancel(locate, ts, id, 10);
            } else if (pick < 992) {
                itch.replace(locate, ts, id, m + 1, 100, price);
            } else if (pick < 997) {
                itch.executed(locate, ts, id, 10, m);
            } else if (pick < 998) {
                itch.executed_with_price(locate, ts, id, 10, m, true, price);
            } else {
                itch.trade(locate, ts, 0, side, 100, stock, price, m);
            }
            if (m % 5 == 4) {
                itch.system_event(0, ts, 'Q');
            }
        }
        return itch;
    }();
    return stream;
}

// One pass over itch_stream() per iteration; Time is per framed message.
// Arg 1: every symbol decoded (--all); 0: one symbol, the rest filtered by locate.
void BM_ItchSourceNext(benchmark::State& state)
{
    using namespace std::chrono_literals;
    const bool all = state.range(0) != 0;
    const ItchSourceConfig config{
        .ticker = all ? "" : "S042", .all_tickers = all, .session_date = 2019y / 12 / 30, .utc_offset = -5h};
    const std::span<const std::byte> bytes = itch_stream().bytes();

    for (auto _ : state) {
        InstrumentRegistry registry{kSymbols};
        ItchSource source{bytes, config, registry};
        MarketEvent event;

        const clock_type::time_point start = clock_type::now();
        while (source.next(event) == SourceResult::Event) {
            benchmark::DoNotOptimize(event);
        }
        const clock_type::time_point stop = clock_type::now();

        const auto messages = static_cast<double>(source.statistics().messages);
        state.SetIterationTime(seconds(stop - start) / messages);
        if (source.error() != SourceError::None) {
            state.SkipWithError("the stream failed to decode");
        }
    }
}

// Release a random live order and acquire a new one, on a half-full pool.
void BM_PoolReleaseAcquire(benchmark::State& state)
{
    const auto capacity = static_cast<std::size_t>(state.range(0));
    OrderPool pool{capacity};
    Flow flow;
    std::vector<Order*> live(capacity / 2);
    for (Order*& order : live) {
        order = pool.acquire(1, Side::Buy, Price{kMid}, Quantity{100}, 0);
    }
    std::vector<std::size_t> picks(kBatch);

    for (auto _ : state) {
        for (std::size_t& pick : picks) {
            pick = flow.below(live.size());
        }
        bool ok = true;
        const clock_type::time_point start = clock_type::now();
        for (const std::size_t pick : picks) {
            ok &= pool.release(*live[pick]);
            live[pick] = pool.acquire(2, Side::Sell, Price{kMid}, Quantity{100}, 0);
        }
        const clock_type::time_point stop = clock_type::now();
        state.SetIterationTime(seconds(stop - start) / static_cast<double>(kBatch));
        if (!ok) {
            state.SkipWithError("release failed");
        }
    }
}

// 4,096 batches of 256: ~1 M operations per run, fixed so every run does the same work.
constexpr benchmark::IterationCount kIterations = 4'096;

#define BOOK_BENCHMARK(...)                                                                                            \
    BENCHMARK(__VA_ARGS__)->ArgName("depth")->Arg(1'000)->Arg(1'000'000)->UseManualTime()->Iterations(kIterations)

BOOK_BENCHMARK(BM_Add);
BOOK_BENCHMARK(BM_Remove<T::Delete>)->Name("BM_Delete");
BOOK_BENCHMARK(BM_Remove<T::Execute>)->Name("BM_ExecuteFull");
BOOK_BENCHMARK(BM_Reduce<T::Execute>)->Name("BM_ExecutePartial");
BOOK_BENCHMARK(BM_Reduce<T::Cancel>)->Name("BM_Cancel");
BOOK_BENCHMARK(BM_Replace);
BENCHMARK(BM_ItchSourceNext)->ArgName("all")->Arg(1)->Arg(0)->UseManualTime()->Iterations(8);
BENCHMARK(BM_PoolReleaseAcquire)
    ->ArgName("capacity")
    ->Arg(1'024)
    ->Arg(4'194'304)
    ->UseManualTime()
    ->Iterations(kIterations);

} // namespace
} // namespace simulator
