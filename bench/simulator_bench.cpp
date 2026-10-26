// Micro-benchmarks of the replay hot paths (cmake -DCHRONOS_BENCH=ON): MarketBook::apply
// per event type at depth 1,000 (in cache) and 1,000,000 (not) for each level backend
// (book:0 list, book:1 ladder), a churn of levels around a moving touch, the backend chosen
// at run time against one fixed at compile time, ItchSource::next, and OrderPool release +
// acquire, as the mean ns per operation of a manually timed batch; the CPU column also counts
// untimed setup, so it is not comparable. BM_EventLoop is the exception: ms per EventLoop run
// over 1,000,000 events.

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
#include "simulator/engine/event_loop.hpp"
#include "simulator/itch/itch_source.hpp"
#include "simulator/market/instrument_registry.hpp"
#include "simulator/market/market_book.hpp"
#include "simulator/market/vector_source.hpp"
#include "simulator/orders/order_book.hpp"
#include "simulator/orders/order_pool.hpp"
#include "simulator/portfolio/portfolio.hpp"
#include "simulator/strategies/market_maker.hpp"

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

BookBackend backend(const benchmark::State& state)
{
    return state.range(0) == 0 ? BookBackend::List : BookBackend::Ladder;
}

// One book of `state`'s backend pre-filled to its depth of orders, whose ids `live` keeps.
struct FilledBook
{
    Flow flow;
    MarketBook book;
    std::vector<OrderId> live;

    explicit FilledBook(const benchmark::State& state)
        : FilledBook(static_cast<std::size_t>(state.range(1)), backend(state))
    {}

    FilledBook(std::size_t depth, BookBackend backend)
        : book{0, depth + 2 * kBatch, backend}
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
    FilledBook b{state};
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
    FilledBook b{state};
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
    FilledBook b{state};
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
    FilledBook b{state};
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

// 4,096 batches of 256: ~1 M operations per run, fixed so every run does the same work.
constexpr benchmark::IterationCount kIterations = 4'096;

// kIterations batches of a book of 1,000 orders that keeps its size: each event an Add, by turns near the touch and
// far from it, or a Delete of a random resting order. The touch drifts a tick at a time and, every 50 batches, jumps
// 500 ticks, so the ladder recentres (spec §5.1). One stream for both backends.
const std::vector<MarketEvent>& churn_stream()
{
    static const std::vector<MarketEvent> events = [] {
        std::mt19937_64 rng{5};
        std::vector<MarketEvent> out;
        std::vector<OrderId> live;
        std::int64_t touch = kMid;
        OrderId next_id = 1;
        auto add = [&](std::int64_t offset) {
            const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
            out.push_back({.order_id = next_id,
                           .price = Price{side == Side::Buy ? touch - offset : touch + kTick + offset},
                           .quantity = Quantity{100},
                           .type = T::Add,
                           .side = side});
            live.push_back(next_id++);
        };
        while (live.size() < 1'000) {
            add(static_cast<std::int64_t>(rng() % 20) * kTick);
        }
        for (std::size_t i = 0; i < std::size_t{kIterations} * kBatch; ++i) {
            if (i % kBatch == 0) {
                touch += (i / kBatch) % 50 == 49 ? (rng() % 2 == 0 ? 500 : -500) * kTick
                                                 : (static_cast<std::int64_t>(rng() % 3) - 1) * kTick;
            }
            if (i % 2 == 0) {
                const std::uint64_t reach = rng() % 4;
                add(static_cast<std::int64_t>(reach == 0 ? 200 + rng() % 2'000 : rng() % 20) * kTick);
            } else {
                const std::size_t k = rng() % live.size();
                out.push_back({.order_id = live[k], .type = T::Delete});
                live[k] = live.back();
                live.pop_back();
            }
        }
        return out;
    }();
    return events;
}

// The churn: kIterations timed batches after the 1,000 Adds that fill the book.
void BM_Churn(benchmark::State& state)
{
    const std::vector<MarketEvent>& events = churn_stream();
    const std::size_t fill = events.size() - std::size_t{kIterations} * kBatch;
    MarketBook book{0, 4'096, backend(state)};
    for (std::size_t i = 0; i < fill; ++i) {
        book.apply(events[i]);
    }
    std::size_t next = fill;
    for (auto _ : state) {
        std::size_t rejected = 0;
        const clock_type::time_point start = clock_type::now();
        for (std::size_t i = 0; i < kBatch; ++i) {
            rejected += book.apply(events[next + i]) != ApplyResult::Applied;
        }
        const clock_type::time_point stop = clock_type::now();
        next += kBatch;
        state.SetIterationTime(seconds(stop - start) / static_cast<double>(kBatch));
        if (rejected != 0) {
            state.SkipWithError("an event was rejected");
        }
    }
}

// The order book of the churn, its backend chosen at run time (Dispatch::Selected, as the engine runs) or fixed at
// compile time (spec §2.4): the two should time alike.
template <Dispatch D>
void BM_Dispatch(benchmark::State& state)
{
    const std::vector<MarketEvent>& events = churn_stream();
    const std::size_t fill = events.size() - std::size_t{kIterations} * kBatch;
    BookStorage storage{4'096, backend(state)};
    BasicOrderBook<D> book{0, storage};
    std::vector<Order*> orders(events.size() + 1);  // by id; the stream's ids rise from 1
    auto apply = [&](const MarketEvent& event) {
        if (event.type == T::Delete) {
            const bool erased = book.erase(event.order_id);
            storage.orders.release(*orders[event.order_id]);
            return erased;
        }
        Order* order = storage.orders.acquire(event.order_id, event.side, event.price, event.quantity, 0);
        orders[event.order_id] = order;
        return order->activate() && book.add(*order);
    };
    for (std::size_t i = 0; i < fill; ++i) {
        apply(events[i]);
    }
    std::size_t next = fill;
    for (auto _ : state) {
        bool ok = true;
        const clock_type::time_point start = clock_type::now();
        for (std::size_t i = 0; i < kBatch; ++i) {
            ok &= apply(events[next + i]);
        }
        const clock_type::time_point stop = clock_type::now();
        next += kBatch;
        state.SetIterationTime(seconds(stop - start) / static_cast<double>(kBatch));
        if (!ok) {
            state.SkipWithError("an event was refused");
        }
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

// n valid events 1 us apart: adds off the mid (never crossed), interleaved with partial executes and deletes.
std::vector<MarketEvent> loop_stream(std::size_t n)
{
    std::mt19937_64 rng{7};
    std::vector<MarketEvent> events;
    events.reserve(n);
    std::vector<OrderId> live;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < n; ++i) {
        const auto ts = static_cast<Timestamp>(i + 1) * 1'000;
        const std::uint64_t roll = rng() % 4;
        if (live.size() < 64 || roll < 2) {
            const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
            const auto offset = static_cast<std::int64_t>(1 + rng() % 20) * kTick;
            events.push_back(MarketEvent{.timestamp = ts,
                                         .order_id = next_id,
                                         .price = Price{side == Side::Buy ? kMid - offset : kMid + offset},
                                         .quantity = Quantity{kShares},
                                         .symbol = 0,
                                         .type = T::Add,
                                         .side = side});
            live.push_back(next_id++);
            continue;
        }
        const std::size_t k = rng() % live.size();
        if (roll == 2) {
            events.push_back(MarketEvent{
                .timestamp = ts, .order_id = live[k], .quantity = Quantity{100}, .symbol = 0, .type = T::Execute});
        } else {
            events.push_back(MarketEvent{.timestamp = ts, .order_id = live[k], .symbol = 0, .type = T::Delete});
            live[k] = live.back();
            live.pop_back();
        }
    }
    return events;
}

// EventLoop over 1,000,000 events with NullStrategy (mm:0) or a quoting MarketMaker (mm:1).
void BM_EventLoop(benchmark::State& state)
{
    static const std::vector<MarketEvent> events = loop_stream(1'000'000);
    const Instrument equity{.price_scale = 4, .quantity_scale = 0};
    const RiskLimits limits{
        .max_position = 1'000'000, .max_order_quantity = Quantity{1'000'000}, .max_open_orders = 16};

    LoopReport report;
    for (auto _ : state) {
        VectorSource source{events};
        MarketBook book{0, std::size_t{1} << 20};
        ExecutionSimulator execution;
        Portfolio portfolio{equity, limits, FeeSchedule{}};

        const auto start = clock_type::now();
        if (state.range(0) == 1) {
            MarketMaker strategy{MarketMakerParams{.requote_cooldown_ns = 1'000}};
            EventLoop<MarketMaker> loop{source,    book,
                                        execution, portfolio,
                                        strategy,  LatencyConfig{.order_latency = 50'000, .report_latency = 50'000}};
            report = loop.run();
        } else {
            NullStrategy strategy;
            EventLoop<NullStrategy> loop{source, book, execution, portfolio, strategy, LatencyConfig{}};
            report = loop.run();
        }
        state.SetIterationTime(seconds(clock_type::now() - start));

        if (!report.replay.succeeded() || report.replay.rejected() != 0) {
            state.SkipWithError("the benchmark stream must replay cleanly");
            break;
        }
        benchmark::DoNotOptimize(book.digest());
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(events.size()));
    state.counters["sim_events"] = static_cast<double>(report.sim_events);
}

#define BOOK_BENCHMARK(...)                                                                                            \
    BENCHMARK(__VA_ARGS__)                                                                                             \
        ->ArgNames({"book", "depth"})                                                                                  \
        ->ArgsProduct({{0, 1}, {1'000, 1'000'000}})                                                                    \
        ->UseManualTime()                                                                                              \
        ->Iterations(kIterations)

BOOK_BENCHMARK(BM_Add);
BOOK_BENCHMARK(BM_Remove<T::Delete>)->Name("BM_Delete");
BOOK_BENCHMARK(BM_Remove<T::Execute>)->Name("BM_ExecuteFull");
BOOK_BENCHMARK(BM_Reduce<T::Execute>)->Name("BM_ExecutePartial");
BOOK_BENCHMARK(BM_Reduce<T::Cancel>)->Name("BM_Cancel");
BOOK_BENCHMARK(BM_Replace);
BENCHMARK(BM_Churn)->ArgName("book")->Arg(0)->Arg(1)->UseManualTime()->Iterations(kIterations);
BENCHMARK(BM_Dispatch<Dispatch::Selected>)
    ->Name("BM_DispatchSelected")
    ->ArgName("book")
    ->Arg(0)
    ->Arg(1)
    ->UseManualTime()
    ->Iterations(kIterations);
BENCHMARK(BM_Dispatch<Dispatch::List>)
    ->Name("BM_DispatchFixed")
    ->ArgName("book")
    ->Arg(0)
    ->UseManualTime()
    ->Iterations(kIterations);
BENCHMARK(BM_Dispatch<Dispatch::Ladder>)
    ->Name("BM_DispatchFixed")
    ->ArgName("book")
    ->Arg(1)
    ->UseManualTime()
    ->Iterations(kIterations);
BENCHMARK(BM_ItchSourceNext)->ArgName("all")->Arg(1)->Arg(0)->UseManualTime()->Iterations(8);
BENCHMARK(BM_PoolReleaseAcquire)
    ->ArgName("capacity")
    ->Arg(1'024)
    ->Arg(4'194'304)
    ->UseManualTime()
    ->Iterations(kIterations);
BENCHMARK(BM_EventLoop)->ArgName("mm")->Arg(0)->Arg(1)->UseManualTime()->Unit(benchmark::kMillisecond)->Iterations(5);

} // namespace
} // namespace simulator
