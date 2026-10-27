#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "chronos/server/itch_input.hpp"
#include "chronos/server/replay_session.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;

TEST(ReplaySessionTest, PlayAppliesTheEventsPacedTimeReachesAndWakesForTheNext)
{
    const std::vector<MarketEvent> events{add(kSecond, 1, Side::Buy, 999'900, 100),
                                          add(2 * kSecond, 2, Side::Sell, 1'000'000, 100),
                                          add(5 * kSecond, 3, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    EXPECT_EQ(session.clock(), kSecond - 1);  // just before the first event
    ASSERT_EQ(session.control(kPlay).error, ErrorCode::None);

    EXPECT_EQ(session.tick(kWall), kWall + 1);
    EXPECT_EQ(session.tick(kWall + 1), kWall + kSecond + 1);
    EXPECT_EQ(session.loop_report().replay.events, 1u);
    EXPECT_EQ(session.clock(), kSecond);

    EXPECT_EQ(session.tick(kWall + 2 * kSecond), kWall + 4 * kSecond + 1);
    EXPECT_EQ(session.loop_report().replay.events, 2u);
    EXPECT_EQ(session.clock(), 3 * kSecond - 1);  // paced time between events

    ASSERT_EQ(session.control(Control{.type = ControlType::Speed, .speed = 10}).error, ErrorCode::None);
    EXPECT_EQ(session.tick(kWall + 2 * kSecond), kWall + 2 * kSecond + 200'000'001);  // (2 s + 1 ns) / 10, rounded up
    EXPECT_EQ(session.tick(kWall + 2 * kSecond + 200'000'001), ReplaySession::never);
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(session.loop_report().replay.events, 3u);
}

TEST(ReplaySessionTest, OrderingAtOneTimestampIsMarketThenSimulatorThenCommand)
{
    // At 200 the ask is deleted, the first IOC arrives and the second command is applied.
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100), erase(200, 1),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config({.order_latency = 100, .report_latency = 0}), over(events)};
    session.control(step(1));
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);
    session.tick(kWall);
    session.control(step(1));
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);
    session.tick(kWall);

    EXPECT_EQ(trace(session),
              (std::vector<std::string>{
                  "applied cmd1 @100 ev1",
                  "report ord1 expired 10 @200",  // the delete at 200 ran first
                  "accepted cmd1",
                  "applied cmd2 @200 ev2",  // after the arrival at 200
              }));
}

TEST(ReplaySessionTest, APausedOrderWaitsAndPlayDeliversItAtItsLatency)
{
    const std::vector<MarketEvent> events{add(kSecond, 1, Side::Sell, 1'000'000, 100),
                                          add(10 * kSecond, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config({.order_latency = 100'000, .report_latency = 0}), over(events)};
    session.control(step(1));
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.tick(kWall + 10 * kSecond), ReplaySession::never);  // paused: time does not move
    EXPECT_EQ(trace(session), (std::vector<std::string>{"applied cmd1 @1000000000 ev1"}));

    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall + 10 * kSecond), kWall + 10 * kSecond + 100'000);
    session.tick(kWall + 10 * kSecond + 100'000);
    EXPECT_EQ(
        trace(session),
        (std::vector<std::string>{"applied cmd1 @1000000000 ev1", "report ord1 fill 10 @1000100000", "accepted cmd1"}));
}

TEST(ReplaySessionTest, AnArrivalInAQuietPeriodRunsBeforeTheNextMarketEvent)
{
    const std::vector<MarketEvent> events{add(kSecond, 1, Side::Sell, 1'000'000, 100),
                                          add(60 * kSecond, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config({.order_latency = 100'000, .report_latency = 50'000}), over(events)};
    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall), kWall + 1);
    EXPECT_EQ(session.tick(kWall + 1), kWall + 59 * kSecond + 1);

    session.submit(ioc_buy(10), 0);
    EXPECT_EQ(session.tick(kWall + kMs), kWall + 1'100'000);  // wakes for the arrival, not the event at 60 s
    EXPECT_EQ(session.tick(kWall + 1'100'000), kWall + 1'150'000);  // then for the report
    EXPECT_EQ(session.tick(kWall + 1'150'000), kWall + 59 * kSecond + 1);
    EXPECT_EQ(session.loop_report().replay.events, 1u);
    EXPECT_EQ(
        trace(session),
        (std::vector<std::string>{"applied cmd1 @1000999999 ev1", "report ord1 fill 10 @1001099999", "accepted cmd1"}));
}

TEST(ReplaySessionTest, ACommandBatchIsAppliedWholeBeforeAnyEventItCreates)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};  // zero latency
    session.control(step(1));
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);
    session.submit(ioc_buy(20), 0);
    session.tick(kWall);
    EXPECT_EQ(trace(session), (std::vector<std::string>{"applied cmd1 @100 ev1", "applied cmd2 @100 ev1"}));
    EXPECT_EQ(session.portfolio().position(), 0);  // no arrival ran inside the batch
    std::vector<std::uint64_t> batches;
    for (const Record& r : records(session)) {
        if (r.kind == RecordKind::Lifecycle && r.step == Lifecycle::Applied) {
            batches.push_back(r.batch_seq);
        }
    }
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0], batches[1]);

    session.control(kPlay);
    session.tick(kWall);
    EXPECT_EQ(trace(session),
              (std::vector<std::string>{"applied cmd1 @100 ev1", "applied cmd2 @100 ev1", "report ord1 fill 10 @100",
                                        "accepted cmd1", "report ord2 fill 20 @100", "accepted cmd2"}));
    EXPECT_EQ(session.portfolio().position(), 30);
}

TEST(ReplaySessionTest, APausedOrderThenStepArrivesBeforeTheLaterMarketEvent)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100), erase(500, 1)};
    ReplaySession session{config({.order_latency = 100, .report_latency = 0}), over(events)};
    session.control(step(1));
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);
    session.tick(kWall);
    session.control(step(1));
    session.tick(kWall);
    EXPECT_EQ(trace(session),
              (std::vector<std::string>{"applied cmd1 @100 ev1", "report ord1 fill 10 @200",
                                        "accepted cmd1"}));  // filled before the delete at 500
    EXPECT_EQ(session.loop_report().replay.events, 2u);
    EXPECT_EQ(session.state(), SessionState::Ended);
}

TEST(ReplaySessionTest, EndOfStreamDrainsExactlyLikeTheHeadlessRun)
{
    // ImbalanceTaker buys at 200; its IOC arrives at 1,200 and the fill is delivered at 2,200, after the last event.
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 300),
                                          add(200, 2, Side::Sell, 1'000'000, 100)};
    const ImbalanceTakerParams params{.threshold = 3.0, .clip = 10, .max_position = 500, .cooldown_ns = 0};
    const SessionConfig cfg = config({.order_latency = 1'000, .report_latency = 1'000});

    VectorSource source{events};
    MarketBook book{kSymbol, 64};
    ExecutionSimulator execution;
    Portfolio portfolio{cfg.instrument, cfg.defaults.limits, cfg.defaults.fees};
    ImbalanceTaker strategy{params};
    EventLoop<ImbalanceTaker> headless{source, book, execution, portfolio, strategy, cfg.defaults.latency};
    const LoopReport expected = headless.run();
    ASSERT_EQ(expected.drained_sim_events, 2u);

    ReplaySession session{cfg, over(events)};
    session.submit(Command{.generation = 1,
                           .type = CommandType::Attach,
                           .strategy = {.kind = SessionStrategyKind::ImbalanceTaker, .imbalance = params}},
                   0);
    session.tick(kWall);  // attached before the first event
    session.control(kFullSpeed);
    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(session.loop_report(), expected);
    EXPECT_EQ(session.book().digest(), book.digest());
    EXPECT_EQ(session.portfolio().position(), 10);
    EXPECT_EQ(session.portfolio().cash(), portfolio.cash());
}

TEST(ReplaySessionTest, AdmissionRefusesBadRequestsStaleGenerationsAFullQueueAndAnEndedSession)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100)};
    ReplaySession session{config(), over(events)};

    EXPECT_EQ(session.submit(post_only_buy(0, 10), 0).error, ErrorCode::BadRequest);  // a limit order needs a price
    EXPECT_EQ(session.submit(Command{.generation = 1, .type = CommandType::Attach}, 0).error, ErrorCode::BadRequest);
    Command stale = cancel(99);
    stale.generation = 2;
    EXPECT_EQ(session.submit(stale, 0).error, ErrorCode::StaleGeneration);
    EXPECT_EQ(session.control(step(0)).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.control(Control{.type = ControlType::Speed, .speed = 1'001}).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.control(Control{.type = ControlType::Speed, .speed = UINT32_MAX}).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.control(Control{.type = ControlType::Speed, .speed = 1'000}).error, ErrorCode::None);
    Control too_many{.type = ControlType::Symbol};
    too_many.spec.ticker = ticker("ACME");
    too_many.spec.days.count = static_cast<std::uint8_t>(max_days + 1);
    EXPECT_EQ(session.control(too_many).error, ErrorCode::BadRequest);
    Control watch{.type = ControlType::Watch};
    watch.watch.run_id[0] = 'r';
    EXPECT_EQ(session.control(watch).error, ErrorCode::BadRequest);  // no factory of fresh mappings

    for (std::size_t i = 1; i < ReplaySession::control_capacity; ++i) {
        ASSERT_EQ(session.control(kPause).error, ErrorCode::None);
    }
    EXPECT_EQ(session.control(kPause).error, ErrorCode::QueueFull);  // the 65th
    session.tick(kWall);  // applies the 64 and acks each
    std::size_t acks = 0;
    for (const Record& r : records(session)) {
        acks += r.kind == RecordKind::Ack ? 1 : 0;
    }
    EXPECT_EQ(acks, ReplaySession::control_capacity);

    Admission admitted;
    for (std::size_t i = 0; i < ReplaySession::command_capacity; ++i) {
        admitted = session.submit(cancel(99), 0);
        ASSERT_EQ(admitted.error, ErrorCode::None);
    }
    const Admission full = session.submit(cancel(99), 0);
    EXPECT_EQ(full.error, ErrorCode::QueueFull);
    EXPECT_EQ(full.seq, 1'028u);  // every submit gets a command_seq
    // Each admission names the ring seq of its verdict record: Admitted, or RejectedAtAdmission after its Received.
    const std::vector<Record> published = records(session);
    EXPECT_EQ(published.back().seq, full.record_seq);
    EXPECT_EQ(published.back().step, Lifecycle::RejectedAtAdmission);
    EXPECT_EQ(admitted.record_seq, full.record_seq - 2);
    EXPECT_EQ(published[published.size() - 3].step, Lifecycle::Admitted);

    bool on_ring = false;  // a refusal at admission is a lifecycle record with its reason
    for (const Record& r : records(session)) {
        on_ring = on_ring ||
            (r.command.command_seq == 1 && r.step == Lifecycle::RejectedAtAdmission &&
             r.reason == ErrorCode::BadRequest);
    }
    EXPECT_TRUE(on_ring);

    session.control(step(1));
    session.tick(kWall);  // the only event: the session ends, and the queued cancels are refused
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(trace(session).back(), "refused cmd1027 not_ready");
    EXPECT_EQ(session.submit(cancel(99), 0).error, ErrorCode::NotReady);

    // An attach's imbalance threshold is at least 1 and finite, checked before the ended session's refusal.
    const auto attach = [](double threshold) {
        return Command{
            .generation = 1,
            .type = CommandType::Attach,
            .strategy = {.kind = SessionStrategyKind::ImbalanceTaker, .imbalance = {.threshold = threshold}}};
    };
    EXPECT_EQ(session.submit(attach(0.99), 0).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.submit(attach(std::numeric_limits<double>::infinity()), 0).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.submit(attach(std::nan("")), 0).error, ErrorCode::BadRequest);
    EXPECT_EQ(session.submit(attach(1.0), 0).error, ErrorCode::NotReady);  // valid: only the ended session refuses it
}

TEST(ReplaySessionTest, ApplyRefusesNoLiquidityRiskAndManualOrdersUnderAStrategy)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100), add(1'000, 2, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    session.control(step(1));
    session.tick(kWall);
    session.submit(
        Command{.generation = 1, .order_type = OrderType::Market, .side = Side::Buy, .quantity = Quantity{10}}, 0);
    session.submit(post_only_buy(999'800, 501), 0);
    session.submit(
        Command{.generation = 1, .type = CommandType::Attach, .strategy = {.kind = SessionStrategyKind::MarketMaker}},
        0);
    session.submit(post_only_buy(999'800, 10), 0);
    session.tick(kWall);
    EXPECT_EQ(trace(session),
              (std::vector<std::string>{"refused cmd1 no_liquidity", "refused cmd2 risk_order_too_large",
                                        "applied cmd3 @100 ev1", "refused cmd4 not_ready"}));
}

TEST(ReplaySessionTest, SimulatorVerdictsNameWouldCrossNotTradingAndCancelRejected)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100), halt(200),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    session.control(step(1));
    session.tick(kWall);
    session.submit(post_only_buy(1'000'000, 10), 0);  // crosses the ask
    session.submit(cancel(77), 0);  // no such order
    session.tick(kWall);
    session.control(step(1));  // the arrivals run before the halt at 200
    session.tick(kWall);
    session.submit(post_only_buy(999'000, 10), 0);  // arrives while halted
    session.tick(kWall);
    session.control(step(1));
    session.tick(kWall);

    EXPECT_EQ(trace(session),
              (std::vector<std::string>{
                  "applied cmd1 @100 ev1",
                  "applied cmd2 @100 ev1",
                  "report ord1 rejected 10 @100",
                  "rejected cmd1 would_cross",
                  "report ord77 cancel_rejected 0 @100",
                  "rejected cmd2 cancel_rejected",
                  "applied cmd3 @200 ev2",
                  "report ord2 rejected 10 @200",
                  "rejected cmd3 not_trading",
              }));
}

TEST(ReplaySessionTest, TooManyOutstandingVerdictsRefuseQueueFull)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config({.order_latency = 100, .report_latency = 0}), over(events)};
    session.control(step(1));
    session.tick(kWall);
    for (std::size_t i = 0; i < ReplaySession::command_capacity; ++i) {
        session.submit(cancel(77), 0);
    }
    session.tick(kWall);  // all applied; paused, so no verdict has come back
    session.submit(cancel(77), 0);
    session.tick(kWall);
    const std::vector<std::string> lines = trace(session);
    ASSERT_EQ(lines.size(), ReplaySession::command_capacity + 1);
    EXPECT_EQ(lines[ReplaySession::command_capacity - 1], "applied cmd1024 @100 ev1");
    EXPECT_EQ(lines.back(), "refused cmd1025 queue_full");
}

TEST(ReplaySessionTest, AZeroLatencyOrderPlacedMidTimestampWaitsForTheRestOfIt)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100), erase(100, 1),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    session.control(step(1));  // stops between the two events at 100
    session.tick(kWall);
    session.submit(ioc_buy(10), 0);  // arrives at 100
    session.tick(kWall);
    session.control(step(1));
    session.tick(kWall);
    session.control(kPlay);
    session.tick(kWall);
    EXPECT_EQ(trace(session),
              (std::vector<std::string>{"applied cmd1 @100 ev1", "report ord1 expired 10 @100",
                                        "accepted cmd1"}));  // after the delete at 100
}

TEST(ReplaySessionTest, FullSpeedYieldsEveryTickSoAPauseIsSeen)
{
    const std::vector<MarketEvent> events = spaced_bids(10'000);
    ReplaySession session{config(), over(events)};
    session.control(kFullSpeed);
    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall), kWall);  // more is due at once
    EXPECT_EQ(session.loop_report().replay.events, ReplaySession::events_per_tick);
    EXPECT_EQ(session.clock(), 4'097 * kMs - 1);  // clamped to just before the pending 4,097th event
    session.control(kPause);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.loop_report().replay.events, ReplaySession::events_per_tick);
    EXPECT_EQ(session.state(), SessionState::Paused);

    session.control(step(1));
    session.control(step(1));  // two Steps before one tick add up: two events, not one
    session.tick(kWall);
    EXPECT_EQ(session.loop_report().replay.events, ReplaySession::events_per_tick + 2);
    EXPECT_EQ(session.state(), SessionState::Paused);

    session.control(step(5'000));
    session.tick(kWall);  // one tick's budget of the 5,000
    EXPECT_EQ(session.state(), SessionState::Stepping);
    session.control(kPause);
    session.tick(kWall);
    const std::uint64_t before = session.loop_report().replay.events;
    session.control(step(1));
    session.tick(kWall);
    EXPECT_EQ(session.loop_report().replay.events, before + 1);  // the cut-short Step left nothing owed
    EXPECT_EQ(session.state(), SessionState::Paused);
}

TEST(ReplaySessionTest, CommandsQueuedWhenTheStreamEndsAreRefusedNotReady)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100)};
    ReplaySession session{config(), over(events)};
    session.submit(ioc_buy(10), 0);
    session.control(kFullSpeed);
    session.control(kPlay);
    session.tick(kWall);  // plays to the end first, then meets the command
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(trace(session), (std::vector<std::string>{"refused cmd1 not_ready"}));
    EXPECT_EQ(session.submit(ioc_buy(10), 0).error, ErrorCode::NotReady);
}

// A two-day chain played by the session, a few steps then at full speed, ends on the digests of stepping it straight
// through; its last book is Monday's alone, as the Clear resets the book and its sequences.
TEST(ReplaySessionTest, AChainPlayedByTheSessionEqualsSteppingItAndEndsOnTheLastDaysBook)
{
    const ScratchDir dir{"session-chain"};
    write_file(dir.path / "12272019.NASDAQ_ITCH50.bin", acme_day(4).bytes());
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", acme_day(6, 1'000).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    SessionConfig cfg = config({.order_latency = 1'000, .report_latency = 1'000});
    cfg.defaults.ticker = ticker("ACME");
    cfg.defaults.days = {.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2};
    cfg.defaults.strategy = {.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}};
    const SourceFactory factory = chain_factory(library, kEquity, false);

    ReplaySession session{cfg, factory};
    session.control(step(3));
    session.tick(kWall);
    session.control(kFullSpeed);
    session.control(kPlay);
    for (Timestamp wall = kWall + snapshot_interval; session.state() != SessionState::Ended;
         wall += snapshot_interval) {
        session.tick(wall);
    }
    EXPECT_EQ(latest(session).progress, 1.0);
    session.close();
    const Record end = records(session).back();
    ASSERT_EQ(end.kind, RecordKind::GenerationEnd);

    Generation straight{cfg, cfg.defaults, factory(cfg.defaults.ticker, cfg.defaults.days)};
    Timestamp ts{0};
    while (straight.loop.peek(ts) == PeekStatus::Event && server::step(straight.loop).ok) {
    }
    straight.loop.finish();
    EXPECT_EQ(end.digests, digests(straight));
    EXPECT_EQ(straight.loop.report().replay.events, 7u + 1u + 9u);  // Friday, the Clear, Monday
    EXPECT_GT(straight.execution.statistics().accepted_quantity, 0u);  // the market maker quoted

    RunSpec monday = cfg.defaults;
    monday.days = {.day = {2019y / 12 / 30}, .count = 1};
    Generation alone{cfg, monday, factory(monday.ticker, monday.days)};
    while (alone.loop.peek(ts) == PeekStatus::Event && server::step(alone.loop).ok) {
    }
    EXPECT_EQ(alone.book.digest(), straight.book.digest());
    EXPECT_NE(straight.book.digest(), MarketBook(kSymbol, 16).digest());
}

// The market maker's quotes rest when Monday's input ends at 09:30:04, before its
// 16:00 close; once the session has played to the end, the close timer has cancelled them with no market event after.
TEST(ReplaySessionTest, TheLastDaysCloseCancelsTheQuotesWhenItsInputEndsFirst)
{
    const ScratchDir dir{"session-close"};
    write_file(dir.path / "12302019.NASDAQ_ITCH50.bin", acme_day(4).bytes());
    DayLibrary library{dir.path, dir.path / "fingerprints.json"};
    library.scan();
    SessionConfig cfg = config({.order_latency = kMs, .report_latency = kMs});
    cfg.defaults.ticker = ticker("ACME");
    cfg.defaults.days = {.day = {2019y / 12 / 30}, .count = 1};
    cfg.defaults.window = TradingWindow::Regular;
    cfg.defaults.strategy = {.kind = SessionStrategyKind::MarketMaker, .market_maker = {.quote_size = 10}};

    ReplaySession session{cfg, chain_factory(library, kEquity, false)};
    session.control(kFullSpeed);
    session.control(kPlay);
    for (Timestamp wall = kWall; session.state() != SessionState::Ended; wall += snapshot_interval) {
        session.tick(wall);
    }
    const std::string close = std::to_string(session_midnight_ns(2019y / 12 / 30, -5h) + 57'600 * kSecond + kMs);
    std::vector<std::string> cancels;
    for (const std::string& line : trace(session)) {
        if (line.find("cancelled") != std::string::npos) {
            cancels.push_back(line);
        }
    }
    EXPECT_EQ(cancels,
              (std::vector<std::string>{"report ord1 cancelled 10 @" + close, "report ord2 cancelled 10 @" + close}));
    EXPECT_TRUE(session.execution().resting_orders().empty());
    EXPECT_EQ(session.portfolio().open_orders(), 0u);
}

}  // namespace
}  // namespace simulator::server::test
