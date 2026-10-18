#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "chronos/server/command_log.hpp"
#include "chronos/server/replay_session.hpp"
#include "chronos/server/session_runner.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path)
{
    std::ifstream in{path};
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

// Waits until the writer has taken every queued record, so a test never outruns it.
void wait_until_written(const LogQueue& queue)
{
    ASSERT_TRUE(eventually([&] { return queue.size() == 0 || queue.failed(); }));
}

// The lines a session logs for one post-only buy it applies: the header, Received, Admitted and Applied.
std::vector<std::string> sample_log(const SessionConfig& cfg)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100)};
    const Scratch scratch{"sample"};
    {
        LogQueue queue;
        const LogWriter writer{queue, scratch.path, cfg, false};
        ReplaySession session{cfg, over(events), &queue};
        session.submit(post_only_buy(999'800, 10), 42);
        session.tick(kWall);
    }
    return read_lines(scratch.path);
}

// Writes `lines`, then `tail`: what a crash leaves of the line it cut off.
void write_log(const std::filesystem::path& path, const std::vector<std::string>& lines, const std::string& tail = "")
{
    std::ofstream out{path};
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    out << tail;
}

// read_log's error for the log at `path`, which it must refuse, leaving `logged` empty.
std::string refusal(const std::filesystem::path& path, std::uint64_t generation = 1)
{
    LoggedGeneration logged;
    std::string error;
    EXPECT_FALSE(read_log(path, generation, logged, error));
    EXPECT_TRUE(logged.applied.empty());
    return error;
}

struct Outcome
{
    std::uint64_t digest{0};
    ExecutionStatistics execution{};
    std::vector<std::uint64_t> resting{};  // id, remaining and ahead_quantity of each resting order
    std::int64_t position{0};
    std::int64_t cash{0};
    std::int64_t fees{0};
    std::int64_t pnl{0};
    std::int64_t max_drawdown{0};
    LoopReport loop{};

    bool operator==(const Outcome&) const = default;
};

Outcome outcome(const MarketBook& book, const ExecutionSimulator& execution, const Portfolio& portfolio,
                const LoopReport& loop)
{
    Outcome o{.digest = book.digest(),
              .execution = execution.statistics(),
              .position = portfolio.position(),
              .cash = portfolio.cash(),
              .fees = portfolio.fees(),
              .pnl = portfolio.pnl(),
              .max_drawdown = portfolio.max_drawdown(),
              .loop = loop};
    for (const ShadowOrder& order : execution.resting_orders()) {
        o.resting.insert(o.resting.end(), {order.id, order.remaining, order.ahead_quantity});
    }
    return o;
}

// The commands and controls sent before tick `t`, priced from the book as it is then.
void script(ReplaySession& session, int t)
{
    LevelInfo bid;
    LevelInfo ask;
    session.book().best(Side::Buy, bid);
    session.book().best(Side::Sell, ask);
    const auto order = [&](OrderType type, Side side, Price price, std::uint64_t quantity) {
        session.submit(
            Command{.generation = 1, .order_type = type, .side = side, .price = price, .quantity = Quantity{quantity}},
            t);
    };
    switch (t) {
    case 5:
        order(OrderType::PostOnly, Side::Buy, bid.price, 10);
        break;
    case 6:  // two commands in one batch
        order(OrderType::Ioc, Side::Sell, bid.price, 10);
        order(OrderType::Market, Side::Buy, Price{}, 20);
        break;
    case 10:
        session.control(kPause);
        break;
    case 11:  // applied while paused, then three steps that end between two events of one timestamp
        order(OrderType::PostOnly, Side::Sell, ask.price, 10);
        session.submit(cancel(1), t);
        session.control(step(3));
        break;
    case 12:
        order(OrderType::Ioc, Side::Buy, ask.price, 10);
        session.control(kPlay);
        break;
    case 40:
        session.submit(
            Command{.generation = 1,
                    .type = CommandType::Attach,
                    .strategy = {.kind = SessionStrategyKind::MarketMaker,
                                 .market_maker = {.quote_size = 10, .max_position = 1'000, .requote_cooldown_ns = 0}}},
            t);
        break;
    case 60:  // a manual order while the strategy is attached: refused at apply, not_ready
        order(OrderType::PostOnly, Side::Buy, bid.price, 10);
        break;
    case 150:
        session.submit(Command{.generation = 1, .type = CommandType::Detach}, t);
        break;
    case 160:
        order(OrderType::Market, Side::Sell, Price{}, 30);
        break;
    case 165:  // still attached when the session restarts
        session.submit(
            Command{.generation = 1,
                    .type = CommandType::Attach,
                    .strategy =
                        {.kind = SessionStrategyKind::ImbalanceTaker,
                         .imbalance = {.threshold = 1.5, .clip = 10, .max_position = 1'000, .cooldown_ns = 1'000'000}}},
            t);
        break;
    case 170:
        session.control(kFullSpeed);
        break;
    default:
        break;
    }
}

TEST(CommandLogTest, ReplayingTheLogReproducesTheBookExecutionAndPortfolio)
{
    const std::vector<MarketEvent> events = random_stream(3'000);
    const SessionConfig cfg = config({.order_latency = 1'500'000, .report_latency = 500'000});
    const Scratch scratch{"reproduce"};
    const std::filesystem::path& path = scratch.path;
    Outcome first;
    Outcome second;
    std::size_t applied_first = 0;
    std::size_t applied_second = 0;
    std::size_t not_ready = 0;
    {
        LogQueue queue;
        const LogWriter writer{queue, path, cfg, false};
        ReplaySession session{cfg, over(events), &queue};
        session.control(kPlay);
        int t = 0;
        for (; session.state() != SessionState::Ended; ++t) {
            script(session, t);
            session.tick(kWall + t * 7 * kMs);
            wait_until_written(queue);
        }
        first = outcome(session.book(), session.execution(), session.portfolio(), session.loop_report());

        // Generation 2 starts with the strategy attached, and detaches it part-way.
        session.control(Control{.type = ControlType::Restart});
        session.control(Control{.type = ControlType::Speed, .speed = 1});
        session.control(kPlay);
        for (int u = 0; u == 0 || session.state() != SessionState::Ended; ++u, ++t) {  // the first tick restarts
            if (u == 100) {
                session.submit(Command{.generation = 2, .type = CommandType::Detach}, t);
            }
            session.tick(kWall + t * 7 * kMs);
            wait_until_written(queue);
        }
        EXPECT_FALSE(queue.failed());
        second = outcome(session.book(), session.execution(), session.portfolio(), session.loop_report());
        session.close();  // ended: the trailer changes nothing
        for (const Record& r : records(session)) {
            const bool applied = r.kind == RecordKind::Lifecycle && r.step == Lifecycle::Applied;
            applied_first += applied && r.generation == 1 ? 1 : 0;
            applied_second += applied && r.generation == 2 ? 1 : 0;
            not_ready += r.kind == RecordKind::Lifecycle && r.step == Lifecycle::RejectedAtApply &&
                    r.reason == ErrorCode::NotReady
                ? 1
                : 0;
        }
    }
    EXPECT_GT(first.execution.maker_fills, 0u);
    EXPECT_GT(first.execution.taker_fills, 0u);
    EXPECT_GT(second.execution.taker_fills, 0u);  // the strategy it started with trades
    EXPECT_EQ(not_ready, 1u);

    LoggedGeneration logged;
    std::string error;
    ASSERT_TRUE(read_log(path, 1, logged, error)) << error;
    EXPECT_TRUE(logged.start == StrategySpec{});
    EXPECT_EQ(logged.applied.size(), applied_first);
    ASSERT_TRUE(logged.end.has_value());  // the restart's trailer
    EXPECT_EQ(logged.end->market_event_seq, 3'000u);
    Generation fresh{cfg, over(events)(name(logged.ticker)), StrategySpec{}};
    ASSERT_TRUE(reproduce(fresh, logged));
    EXPECT_EQ(outcome(fresh.book, fresh.execution, fresh.portfolio, fresh.loop.report()), first);

    ASSERT_TRUE(read_log(path, 2, logged, error)) << error;
    EXPECT_EQ(logged.start.kind, SessionStrategyKind::ImbalanceTaker);
    EXPECT_EQ(logged.applied.size(), applied_second);
    ASSERT_TRUE(logged.end.has_value());  // close()'s trailer
    Generation again{cfg, over(events)(name(logged.ticker)), StrategySpec{}};
    ASSERT_TRUE(reproduce(again, logged));
    EXPECT_EQ(outcome(again.book, again.execution, again.portfolio, again.loop.report()), second);
}

TEST(CommandLogTest, TheLogHasAHeaderPerGenerationAndOneFlushedLinePerRecord)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    const SessionConfig cfg = config({.order_latency = 100'000, .report_latency = 0});
    const Scratch scratch{"format"};
    const std::filesystem::path& path = scratch.path;
    {
        LogQueue queue;
        const LogWriter writer{queue, path, cfg, true};  // also fsyncs every command line
        ReplaySession session{cfg, over(events), &queue};
        session.submit(post_only_buy(999'800, 10), 42);
        session.tick(kWall);
        session.control(Control{.type = ControlType::Symbol, .symbol = ticker("MSFT")});
        session.tick(kWall);
        std::uint64_t last_logged = 0;  // the generation 2 header: an Ack is never logged
        for (const Record& r : records(session)) {
            if (r.kind != RecordKind::Ack) {
                last_logged = r.seq;
            }
        }
        EXPECT_TRUE(eventually([&] { return writer.persisted_seq() == last_logged; }));
    }
    const std::vector<std::string> lines = read_lines(path);
    ASSERT_EQ(lines.size(), 6u);
    const auto has = [&](std::size_t line, const std::string& text) {
        return lines[line].find(text) != std::string::npos;
    };
    EXPECT_TRUE(has(0, "\"type\":\"header\""));
    EXPECT_TRUE(has(0, "\"generation\":1"));
    EXPECT_TRUE(has(0, "\"protocol\":1"));
    EXPECT_TRUE(has(0, "\"session_id\":\"test\""));
    EXPECT_TRUE(has(0, "\"order_ns\":100000"));
    EXPECT_TRUE(has(0, "\"max_order_quantity\":500"));
    EXPECT_TRUE(has(0, "\"book_capacity\":16384"));
    EXPECT_TRUE(has(0, "\"kind\":\"none\""));
    EXPECT_TRUE(has(0, "\"ticker\":\"\""));  // no symbol chosen yet
    EXPECT_TRUE(has(1, "\"seq\":2"));  // in seq order: the header has no seq
    EXPECT_TRUE(has(2, "\"seq\":3"));
    EXPECT_TRUE(has(3, "\"seq\":4"));
    EXPECT_TRUE(has(1, "\"step\":\"received\""));
    EXPECT_TRUE(has(1, "\"received_wall_ts\":42"));
    EXPECT_TRUE(has(2, "\"step\":\"admitted\""));
    EXPECT_TRUE(has(3, "\"step\":\"applied\""));
    EXPECT_TRUE(has(3, "\"exchange_ts\":99"));
    EXPECT_TRUE(has(3, "\"market_event_seq\":0"));
    EXPECT_TRUE(has(3, "\"order_id\":1"));
    EXPECT_TRUE(has(4, "\"type\":\"trailer\""));  // the symbol switch closes generation 1 where it stood
    EXPECT_TRUE(has(4, "\"seq\":5"));
    EXPECT_TRUE(has(4, "\"generation\":1"));
    EXPECT_TRUE(has(4, "\"exchange_ts\":99"));
    EXPECT_TRUE(has(4, "\"market_event_seq\":0"));
    EXPECT_TRUE(has(4, "\"book_digest\":"));
    EXPECT_TRUE(has(5, "\"type\":\"header\""));
    EXPECT_TRUE(has(5, "\"generation\":2"));
    EXPECT_TRUE(has(5, "\"ticker\":\"MSFT\""));
    LoggedGeneration logged;
    std::string error;
    ASSERT_TRUE(read_log(path, 2, logged, error)) << error;
    EXPECT_EQ(name(logged.ticker), "MSFT");
    EXPECT_FALSE(logged.end.has_value());  // generation 2 was never closed
    ASSERT_TRUE(read_log(path, 1, logged, error)) << error;
    ASSERT_TRUE(logged.end.has_value());
    EXPECT_EQ(logged.end->exchange_ts, 99);
}

TEST(CommandLogTest, AReproductionStopsAtTheTrailerAndChecksItsDigests)
{
    const std::vector<MarketEvent> events = random_stream(3'000);
    const SessionConfig cfg = config({.order_latency = 1'500'000, .report_latency = 500'000});
    const Scratch scratch{"trailer"};
    Outcome closed;
    {
        LogQueue queue;
        const LogWriter writer{queue, scratch.path, cfg, false};
        ReplaySession session{cfg, over(events), &queue};
        session.control(kPlay);
        for (int t = 0; t < 30; ++t) {  // orders, a pause, a step mid-timestamp, then play
            script(session, t);
            session.tick(kWall + t * 7 * kMs);
        }
        session.close();  // mid-stream, with orders resting and in flight
        closed = outcome(session.book(), session.execution(), session.portfolio(), session.loop_report());
        wait_until_written(queue);
    }
    LoggedGeneration logged;
    std::string error;
    ASSERT_TRUE(read_log(scratch.path, 1, logged, error)) << error;
    ASSERT_TRUE(logged.end.has_value());
    EXPECT_GT(logged.end->market_event_seq, 0u);
    EXPECT_LT(logged.end->market_event_seq, 3'000u);
    Generation fresh{cfg, over(events)(name(logged.ticker)), StrategySpec{}};
    ASSERT_TRUE(reproduce(fresh, logged));
    EXPECT_EQ(outcome(fresh.book, fresh.execution, fresh.portfolio, fresh.loop.report()), closed);

    LoggedGeneration tampered = logged;  // a trailer whose digest does not match is a failed reproduction
    tampered.end->digests.portfolio ^= 1;
    Generation again{cfg, over(events)(name(logged.ticker)), StrategySpec{}};
    EXPECT_FALSE(reproduce(again, tampered));

    // A switched generation reproduces from its log end to end: the ticker its header names is part of its input.
    std::vector<MarketEvent> bbb = events;  // a second ticker's stream: the same shape, 5,000 ticks higher
    for (MarketEvent& event : bbb) {
        event.price.ticks += event.type == MarketEventType::Add ? 5'000 : 0;
    }
    const SourceFactory by_ticker = [&](std::string_view t) {
        return std::make_unique<VectorSource>(t == "BBB" ? bbb : events);
    };
    const Scratch switched{"switched"};
    Outcome live;
    {
        LogQueue queue;
        const LogWriter writer{queue, switched.path, cfg, false};
        ReplaySession session{cfg, by_ticker, &queue};
        Timestamp wall = kWall;
        const auto run = [&](int ticks) {
            for (int i = 0; i < ticks; ++i) {
                session.tick(wall += 7 * kMs);
            }
        };
        LevelInfo bid;
        LevelInfo ask;
        session.control(kPlay);
        run(10);
        session.book().best(Side::Buy, bid);
        session.submit(post_only_buy(bid.price.ticks, 10), 0);
        run(10);
        session.control(Control{.type = ControlType::Symbol, .symbol = ticker("BBB")});
        run(1);  // the switch: generation 2 replays BBB, paused before its first event
        session.control(kPlay);
        run(10);
        session.book().best(Side::Sell, ask);
        session.submit(Command{.generation = 2,
                               .order_type = OrderType::Ioc,
                               .side = Side::Buy,
                               .price = ask.price,
                               .quantity = Quantity{10}},
                       0);
        run(10);
        session.close();
        live = outcome(session.book(), session.execution(), session.portfolio(), session.loop_report());
        wait_until_written(queue);
    }
    EXPECT_GT(live.execution.taker_fills, 0u);
    ASSERT_TRUE(read_log(switched.path, 2, logged, error)) << error;
    EXPECT_EQ(name(logged.ticker), "BBB");
    EXPECT_EQ(logged.applied.size(), 1u);
    ASSERT_TRUE(logged.end.has_value());
    Generation bbb_again{cfg, by_ticker(name(logged.ticker)), StrategySpec{}};
    ASSERT_TRUE(reproduce(bbb_again, logged));
    EXPECT_EQ(bbb_again.book.digest(), live.digest);
    EXPECT_EQ(outcome(bbb_again.book, bbb_again.execution, bbb_again.portfolio, bbb_again.loop.report()), live);

    Generation wrong{cfg, by_ticker(""), StrategySpec{}};  // the first ticker's stream is not generation 2's input
    EXPECT_FALSE(reproduce(wrong, logged));
    EXPECT_NE(wrong.book.digest(), live.digest);

    ASSERT_TRUE(read_log(switched.path, 1, logged, error)) << error;  // the switch's trailer closed generation 1
    ASSERT_TRUE(logged.end.has_value());
    Generation first_again{cfg, by_ticker(name(logged.ticker)), StrategySpec{}};
    EXPECT_TRUE(reproduce(first_again, logged));
}

TEST(CommandLogTest, AFullLogRefusesNewCommandsAndTheEngineRunsOn)
{
    const std::vector<MarketEvent> events = random_stream(2'000);
    LogQueue queue;  // no writer: nothing drains it
    ReplaySession session{config(), over(events), &queue};
    while (queue.push(Record{})) {
    }
    EXPECT_TRUE(queue.failed());
    EXPECT_EQ(session.submit(cancel(7), 0).error, ErrorCode::LogUnavailable);

    session.control(kFullSpeed);
    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);  // never blocked by the full log
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(session.loop_report().replay.events, 2'000u);
    EXPECT_TRUE(latest(session).metrics.log_failed);
}

// A log above half its capacity holds market work however it is asked for, and only that: controls, commands and
// snapshots go on, and the engine never waits on the writer.
TEST(CommandLogTest, ABackedUpLogHoldsMarketWorkUntilTheWriterDrainsIt)
{
    const std::vector<MarketEvent> events = spaced_bids(10'000);  // no strategy: the events publish no records
    const auto check = [&](const Control& control, SessionState held, std::uint64_t resumed) {
        LogQueue queue;  // no writer: the test drains it
        ReplaySession session{config(), over(events), &queue};
        while (queue.size() <= LogQueue::capacity / 2) {
            queue.push(Record{});
        }
        ASSERT_TRUE(queue.backed_up());
        session.control(kFullSpeed);
        session.control(control);
        session.submit(cancel(7), 0);
        EXPECT_EQ(session.tick(kWall), kWall + snapshot_interval);  // it polls for the writer, it does not spin
        EXPECT_EQ(session.state(), held);
        EXPECT_EQ(session.loop_report().replay.events, 0u);
        EXPECT_EQ(trace(session),
                  (std::vector<std::string>{held == SessionState::Seeking ? "refused cmd1 not_ready"
                                                                          : "applied cmd1 @999999 ev0"}));
        if (held != SessionState::Seeking) {  // a seek publishes no snapshots
            EXPECT_EQ(latest(session).last_command_seq, 1u);
        }
        EXPECT_FALSE(queue.failed());

        Record record;
        while (queue.size() > LogQueue::capacity / 4) {
            queue.pop(record);
        }
        EXPECT_FALSE(queue.backed_up());
        session.tick(kWall);
        EXPECT_EQ(session.loop_report().replay.events, resumed);
        EXPECT_FALSE(queue.failed());
    };
    check(kPlay, SessionState::Playing, ReplaySession::events_per_tick);
    check(step(10), SessionState::Stepping, 10);
    check(Control{.type = ControlType::Seek, .seek_ts = 5'000 * kMs}, SessionState::Seeking,
          ReplaySession::events_per_tick);
}

TEST(CommandLogTest, AMarketMakerAtFullSpeedNeverOverrunsTheLog)
{
    const std::vector<MarketEvent> events = random_stream(20'000);
    const Scratch scratch{"backpressure"};
    LogQueue queue;
    const LogWriter writer{queue, scratch.path, config(), false};
    ReplaySession session{config(), over(events), &queue};
    session.submit(
        Command{.generation = 1,
                .type = CommandType::Attach,
                .strategy = {.kind = SessionStrategyKind::MarketMaker,
                             .market_maker = {.quote_size = 10, .max_position = 1'000, .requote_cooldown_ns = 0}}},
        0);
    session.tick(kWall);  // attached before the first event
    {
        const SessionRunner runner{session};
        session.control(kFullSpeed);
        session.control(kPlay);
        ASSERT_TRUE(eventually([&] { return session.state() == SessionState::Ended; }));
    }
    EXPECT_FALSE(queue.failed());
    EXPECT_EQ(session.loop_report().replay.events, 20'000u);
    std::uint64_t last_logged = 0;  // an Ack is never logged
    for (const Record& r : records(session)) {
        if (r.kind != RecordKind::Ack) {
            last_logged = r.seq;
        }
    }
    EXPECT_GT(last_logged, LogQueue::capacity);  // more records than the queue holds, and the writer took them all
    EXPECT_TRUE(eventually([&] { return writer.persisted_seq() == last_logged; }));
}

TEST(CommandLogTest, ACommandAdmittedBeforeTheLogFilledIsRefusedAtApply)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100)};
    LogQueue queue;
    ReplaySession session{config(), over(events), &queue};
    ASSERT_EQ(session.submit(cancel(7), 0).error, ErrorCode::None);
    while (queue.push(Record{})) {
    }
    session.tick(kWall);
    EXPECT_EQ(trace(session), (std::vector<std::string>{"refused cmd1 log_unavailable"}));
}

TEST(CommandLogTest, AWriterThatCannotOpenItsFileFailsTheLog)
{
    LogQueue queue;
    const LogWriter writer{queue, "/nonexistent-chronos-dir/session.jsonl", config(), false};
    EXPECT_TRUE(queue.failed());
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100)};
    ReplaySession session{config(), over(events), &queue};
    EXPECT_EQ(session.submit(cancel(7), 0).error, ErrorCode::LogUnavailable);
}

TEST(CommandLogTest, ReadLogRefusesATruncatedLine)
{
    const Scratch scratch{"truncated"};
    const std::filesystem::path& path = scratch.path;
    const std::vector<std::string> good = sample_log(config());
    ASSERT_EQ(good.size(), 4u);  // header, received, admitted, applied
    const auto tampered = [&](std::size_t line, const std::string& from, const std::string& to) {
        std::string text = good[line];
        const std::size_t at = text.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        return at == std::string::npos ? text : text.replace(at, from.size(), to);
    };

    // The log as written reads, and a submit thread's Received may come before its generation's header.
    LoggedGeneration logged;
    std::string error;
    write_log(path, good);
    ASSERT_TRUE(read_log(path, 1, logged, error)) << error;
    EXPECT_EQ(logged.applied.size(), 1u);
    EXPECT_TRUE(logged.start == StrategySpec{});
    write_log(path, {good[1], good[0], good[2], good[3]});
    ASSERT_TRUE(read_log(path, 1, logged, error)) << error;
    EXPECT_EQ(logged.applied.size(), 1u);

    // A line cut off by a crash.
    write_log(path, {good[0]}, good[1].substr(0, good[1].size() / 2));
    EXPECT_EQ(refusal(path), "line 2: not JSON");
    write_log(path, {good[0], good[1], good[2]}, good[3].substr(0, 40));
    EXPECT_EQ(refusal(path), "line 4: not JSON");

    // A value read_log would otherwise guess at.
    const std::string unexpected = "unexpected field or value";
    write_log(path, {good[0], tampered(3, "\"side\":\"buy\"", "\"side\":\"sel\"")});
    EXPECT_EQ(refusal(path), "line 2: " + unexpected);
    write_log(path, {good[0], tampered(3, "\"quantity\":10", "\"quantity\":-10")});
    EXPECT_EQ(refusal(path), "line 2: " + unexpected);
    write_log(path, {good[0], tampered(3, "\"quantity\":10", "\"quantity\":10.9")});
    EXPECT_EQ(refusal(path), "line 2: " + unexpected);
    write_log(path, {good[0], tampered(3, "\"type\":\"command\"", "\"type\":\"comment\"")});
    EXPECT_EQ(refusal(path), "line 2: " + unexpected);
    write_log(path, {tampered(0, "\"kind\":\"none\"", "\"kind\":\"mm\"")});
    EXPECT_EQ(refusal(path), "line 1: " + unexpected);
    write_log(path, {tampered(0, "\"ticker\":\"\"", "\"ticker\":\"TOOLONGXX\"")});
    EXPECT_EQ(refusal(path), "line 1: " + unexpected);

    // A record line whose seq does not follow the previous record line's.
    write_log(path, {good[0], good[2], good[1], good[3]});
    EXPECT_EQ(refusal(path), "line 3: seq out of order");
    write_log(path, {good[0], good[1], good[1]});
    EXPECT_EQ(refusal(path), "line 3: seq out of order");

    // A log without its header, with two, or from another protocol.
    write_log(path, {good[1], good[2], good[3]});
    EXPECT_EQ(refusal(path), "no header for generation 1");
    write_log(path, good);
    EXPECT_EQ(refusal(path, 2), "no header for generation 2");
    write_log(path, {good[0], good[0], good[1]});
    EXPECT_EQ(refusal(path), "line 2: a second header for generation 1");
    write_log(path, {tampered(0, "\"protocol\":1", "\"protocol\":2"), good[1]});
    EXPECT_EQ(refusal(path), "line 1: unsupported log protocol");
}

}  // namespace
}  // namespace simulator::server::test
