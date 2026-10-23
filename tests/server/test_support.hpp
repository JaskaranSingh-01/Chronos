#pragma once

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "chronos/server/generation.hpp"
#include "chronos/server/replay_session.hpp"
#include "itch/itch_test_builder.hpp"
#include "simulator/market/market_event.hpp"
#include "simulator/market/vector_source.hpp"

namespace simulator::server::test {

inline constexpr SymbolId kSymbol = 0;
inline constexpr Timestamp kMs = 1'000'000;
inline constexpr Timestamp kSecond = 1'000'000'000;
inline constexpr Instrument kEquity{.price_scale = 4, .quantity_scale = 0};

inline MarketEvent add(Timestamp ts, OrderId id, Side side, std::int64_t price, std::uint64_t quantity)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Add,
                       .side = side};
}

// An Execute of `quantity` from order `id`: ITCH E, or ITCH C with its own `price`.
inline MarketEvent execute(Timestamp ts, OrderId id, std::uint64_t quantity, std::int64_t price = 0)
{
    return MarketEvent{.timestamp = ts,
                       .order_id = id,
                       .price = Price{price},
                       .quantity = Quantity{quantity},
                       .symbol = kSymbol,
                       .type = MarketEventType::Execute};
}

// Zero fees, 16,384 book slots, and risk limits of position 1,000, order 500 and 4 open orders.
inline SessionConfig config(LatencyConfig latency = {})
{
    return SessionConfig{
        .session_id = "test",
        .instrument = kEquity,
        .symbol = kSymbol,
        .book_capacity = 16'384,
        .defaults = {.latency = latency,
                     .limits = {.max_position = 1'000, .max_order_quantity = Quantity{500}, .max_open_orders = 4}}};
}

inline constexpr Timestamp kWall = 1'000 * kSecond;  // the wall clock at a test's first tick

inline MarketEvent erase(Timestamp ts, OrderId id)
{
    return MarketEvent{.timestamp = ts, .order_id = id, .symbol = kSymbol, .type = MarketEventType::Delete};
}

inline MarketEvent halt(Timestamp ts)
{
    return MarketEvent{
        .timestamp = ts, .symbol = kSymbol, .type = MarketEventType::Status, .trading_state = TradingState::Halted};
}

// n one-share bids 1 ms apart from 1 ms, at 900,000 to 900,099.
inline std::vector<MarketEvent> spaced_bids(std::size_t n)
{
    std::vector<MarketEvent> events;
    events.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        events.push_back(add(static_cast<Timestamp>(i + 1) * kMs, i + 1, Side::Buy,
                             900'000 + static_cast<std::int64_t>(i % 100), 1));
    }
    return events;
}

// A factory of VectorSources over `events`, whatever the ticker; the caller keeps `events` alive.
inline SourceFactory over(const std::vector<MarketEvent>& events)
{
    return [&events](const Ticker&, const DayList&) { return std::make_unique<VectorSource>(events); };
}

// A client message of `type` to the session config() names, at `generation`, with `fields` (each with its leading
// comma).
inline std::string client_message(const std::string& type, const std::string& fields = "", std::uint64_t generation = 1)
{
    return R"({"type":")" + type + R"(","session_id":"test","generation":)" + std::to_string(generation) + fields + "}";
}

inline Ticker ticker(std::string_view text)
{
    Ticker t{};
    EXPECT_TRUE(to_ticker(text, t)) << text;
    return t;
}

// Every record on the session's ring, oldest first.
inline std::vector<Record> records(const ReplaySession& session)
{
    std::vector<Record> all;
    std::vector<Record> chunk(4'096);
    for (std::uint64_t from = 1;;) {
        const RecordRing<Record>::Read read = session.reports_since(from, chunk);
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read.count));
        if (read.count < chunk.size()) {
            return all;
        }
        from = all.back().seq + 1;
    }
}

inline std::string report_name(ReportType type)
{
    switch (type) {
    case ReportType::Accepted:
        return "accepted";
    case ReportType::Rejected:
        return "rejected";
    case ReportType::Fill:
        return "fill";
    case ReportType::Cancelled:
        return "cancelled";
    case ReportType::CancelRejected:
        return "cancel_rejected";
    case ReportType::Expired:
        return "expired";
    }
    return "?";
}

// One line per engine-side command step and report, e.g. "applied cmd1 @100 ev1" (clock, market events applied).
inline std::vector<std::string> trace(const ReplaySession& session)
{
    std::vector<std::string> lines;
    for (const Record& r : records(session)) {
        const std::string cmd = "cmd" + std::to_string(r.command.command_seq);
        if (r.kind == RecordKind::Report) {
            lines.push_back("report ord" + std::to_string(r.order_id) + " " + report_name(r.report.type) + " " +
                            std::to_string(r.report.quantity.value()) + " @" + std::to_string(r.report.exchange_time));
        } else if (r.kind != RecordKind::Lifecycle) {
            continue;
        } else if (r.step == Lifecycle::Applied) {
            lines.push_back("applied " + cmd + " @" + std::to_string(r.exchange_ts) + " ev" +
                            std::to_string(r.market_event_seq));
        } else if (r.step == Lifecycle::RejectedAtApply) {
            lines.push_back("refused " + cmd + " " + std::string{to_string(r.reason)});
        } else if (r.step == Lifecycle::SimulatorAccepted) {
            lines.push_back("accepted " + cmd);
        } else if (r.step == Lifecycle::SimulatorRejected) {
            lines.push_back("rejected " + cmd + " " + std::string{to_string(r.reason)});
        }
    }
    return lines;
}

// An IOC buy at 1,000,000 for generation 1.
inline Command ioc_buy(std::uint64_t quantity)
{
    return Command{.generation = 1,
                   .order_type = OrderType::Ioc,
                   .side = Side::Buy,
                   .price = Price{1'000'000},
                   .quantity = Quantity{quantity}};
}

inline Command post_only_buy(std::int64_t price, std::uint64_t quantity)
{
    return Command{.generation = 1, .side = Side::Buy, .price = Price{price}, .quantity = Quantity{quantity}};
}

inline Command cancel(OrderId id)
{
    return Command{.generation = 1, .type = CommandType::Cancel, .order_id = id};
}

inline Control step(std::uint64_t n)
{
    return Control{.type = ControlType::Step, .steps = n};
}

inline constexpr Control kPlay{.type = ControlType::Play};
inline constexpr Control kPause{.type = ControlType::Pause};
inline constexpr Control kFullSpeed{.type = ControlType::Speed, .speed = 0};

// n valid events, two per millisecond from 1 ms: 100-share adds within 20 cents of 100.00 (bids below, asks above),
// partial executes and deletes of live orders. Nothing is rejected and the book never crosses.
inline std::vector<MarketEvent> random_stream(std::size_t n)
{
    std::mt19937_64 rng{7};
    std::vector<MarketEvent> events;
    events.reserve(n);
    std::vector<std::pair<OrderId, std::uint64_t>> live;  // id, shares left
    OrderId next_id = 1;
    for (std::size_t i = 0; i < n; ++i) {
        const Timestamp ts = static_cast<Timestamp>(i / 2 + 1) * kMs;
        const std::uint64_t roll = rng() % 4;
        if (live.size() < 8 || roll < 2) {
            const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
            const auto offset = static_cast<std::int64_t>(1 + rng() % 20) * 100;
            events.push_back(add(ts, next_id, side, side == Side::Buy ? 1'000'000 - offset : 1'000'000 + offset, 100));
            live.emplace_back(next_id++, 100);
            continue;
        }
        const std::size_t k = rng() % live.size();
        if (roll == 2 && live[k].second > 10) {
            events.push_back(execute(ts, live[k].first, 10));
            live[k].second -= 10;
        } else {
            events.push_back(erase(ts, live[k].first));
            live[k] = live.back();
            live.pop_back();
        }
    }
    return events;
}

// A fresh file path in the system temp directory, unique to this process; the file is removed with the object.
struct Scratch
{
    std::filesystem::path path;

    explicit Scratch(const std::string& name)
        : path(std::filesystem::temp_directory_path() /
               ("chronos_server_tests-" + std::to_string(::getpid()) + "-" + name + ".jsonl"))
    {
        std::filesystem::remove(path);
    }

    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

// A directory in the system temp directory, unique to this process; removed with the object.
struct ScratchDir
{
    std::filesystem::path path;

    explicit ScratchDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() /
               ("chronos_server_tests-" + std::to_string(::getpid()) + "-" + name))
    {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }

    ~ScratchDir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

inline void write_file(const std::filesystem::path& path, std::span<const std::byte> bytes)
{
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// Flips a bit of the byte at `at` of `file`, in place: the byte always changes.
inline void flip(const std::filesystem::path& file, std::uint64_t at)
{
    std::fstream edit{file, std::ios::in | std::ios::out | std::ios::binary};
    edit.seekg(static_cast<std::streamoff>(at));
    const char was = static_cast<char>(edit.get());
    edit.seekp(static_cast<std::streamoff>(at));
    edit.put(static_cast<char>(was ^ 0x01));
}

inline constexpr std::uint64_t kNineThirty = 34'200 * kSecond;  // ns after midnight

// An ITCH 5.0 day of one stock, ACME: the start of messages, its directory entry and trading state, the start of
// system hours, and regular hours from 09:30 to 16:00 as the System Events set them. At 09:30 a 10.00 bid and a 10.01
// offer of 1,000 rest, and from 09:30:01 one of them trades 100 a second, `seconds` times; `shift` ticks move every
// price.
inline itch_test::ItchBuilder acme_day(std::uint64_t seconds = 4, std::uint32_t shift = 0)
{
    itch_test::ItchBuilder itch;
    itch.system_event(0, 3 * 3'600 * kSecond, 'O')
        .stock_directory(1, 3 * 3'600 * kSecond, "ACME")
        .trading_action(1, 3 * 3'600 * kSecond, "ACME", 'T')
        .system_event(0, 4 * 3'600 * kSecond, 'S')
        .system_event(0, kNineThirty, 'Q')
        .add_order(1, kNineThirty, 1, 'B', 1'000, "ACME", 100'000 + shift)
        .add_order(1, kNineThirty, 2, 'S', 1'000, "ACME", 100'100 + shift);
    for (std::uint64_t s = 1; s <= seconds; ++s) {
        itch.executed(1, kNineThirty + s * kSecond, s % 2 == 0 ? 1 : 2, 100, s);
    }
    itch.system_event(0, 57'600 * kSecond, 'M');
    return itch;
}

// A busier day of ACME, fixed by `seed`, for Lab runs: a 10.00 bid and 10.01 offer of 1,000 from 09:00; from 09:30, a
// message every 10 ms, `messages` of them: adds up to 3 cents behind either touch, executions (E, or C at the order's
// own price), cancels and deletes. The market maker fills by rules (a) and (d), and the imbalance taker trades.
inline itch_test::ItchBuilder lab_day(std::uint64_t seed, std::size_t messages = 4'000)
{
    struct Live
    {
        std::uint64_t id;
        std::uint32_t price;
        std::uint32_t shares;
    };
    constexpr std::uint64_t hour = 3'600 * kSecond;
    itch_test::ItchBuilder itch;
    itch.system_event(0, 3 * hour, 'O')
        .stock_directory(1, 3 * hour, "ACME")
        .trading_action(1, 3 * hour, "ACME", 'T')
        .system_event(0, 4 * hour, 'S')
        .add_order(1, 9 * hour, 1, 'B', 1'000, "ACME", 100'000)
        .add_order(1, 9 * hour, 2, 'S', 1'000, "ACME", 100'100)
        .system_event(0, kNineThirty, 'Q');
    std::vector<Live> live{{1, 100'000, 1'000}, {2, 100'100, 1'000}};
    std::mt19937_64 rng{seed};
    std::uint64_t next_id = 3;
    std::uint64_t match = 1;
    for (std::uint64_t i = 1; i <= messages; ++i) {
        const std::uint64_t ts = kNineThirty + i * 10 * kMs;
        const std::uint64_t roll = rng() % 10;
        if (live.size() < 4 || roll < 4) {
            const bool buy = rng() % 2 == 0;
            const auto behind = static_cast<std::uint32_t>(rng() % 4 * 100);
            const std::uint32_t price = buy ? 100'000 - behind : 100'100 + behind;
            const auto shares = static_cast<std::uint32_t>(100 * (1 + rng() % 10));
            itch.add_order(1, ts, next_id, buy ? 'B' : 'S', shares, "ACME", price);
            live.push_back({next_id++, price, shares});
            continue;
        }
        const std::size_t k = rng() % live.size();
        Live& order = live[k];
        std::uint32_t part = std::min(order.shares, static_cast<std::uint32_t>(100 * (1 + rng() % 3)));
        if (roll == 9) {
            itch.erase(1, ts, order.id);
            part = order.shares;
        } else if (roll >= 7) {
            itch.cancel(1, ts, order.id, part);
        } else if (roll == 6) {
            itch.executed_with_price(1, ts, order.id, part, match++, true, order.price);
        } else {
            itch.executed(1, ts, order.id, part, match++);
        }
        order.shares -= part;
        if (order.shares == 0) {
            live[k] = live.back();
            live.pop_back();
        }
    }
    itch.system_event(0, 16 * hour, 'M');
    return itch;
}

// The session's latest snapshot; a test failure if none was published.
inline Snapshot latest(const ReplaySession& session)
{
    Snapshot snapshot;
    EXPECT_TRUE(session.read_snapshot([&](const Snapshot& published) { snapshot = published; }));
    return snapshot;
}

// Polls `done` every 100 us for up to 10 s; for work on another thread.
template <class F>
bool eventually(F&& done)
{
    for (int i = 0; i < 100'000; ++i) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }
    return done();
}

}  // namespace simulator::server::test
