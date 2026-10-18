#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <future>
#include <thread>
#include <vector>

#include "chronos/server/replay_session.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

TEST(SnapshotTest, ASnapshotIsOneBoundaryAndItsWatermarksOnlyRise)
{
    const std::vector<MarketEvent> events = random_stream(3'000);
    ReplaySession session{config(), over(events)};
    session.submit(
        Command{.generation = 1,
                .type = CommandType::Attach,
                .strategy = {.kind = SessionStrategyKind::MarketMaker,
                             .market_maker = {.quote_size = 10, .max_position = 1'000, .requote_cooldown_ns = 0}}},
        0);
    session.control(kPlay);

    Snapshot previous;
    std::size_t checked = 0;
    for (Timestamp wall = kWall; session.state() != SessionState::Ended; wall += 10 * kMs) {
        session.tick(wall);
        Snapshot s;
        if (!session.read_snapshot([&](const Snapshot& published) { s = published; }) ||
            s.snapshot_seq == previous.snapshot_seq) {
            continue;
        }
        // Every field is the engine's state now: nothing has run since this tick published it.
        EXPECT_EQ(s.snapshot_seq, previous.snapshot_seq + 1);
        EXPECT_EQ(s.exchange_ts, session.clock());
        EXPECT_EQ(s.event_seq, session.loop_report().replay.events);
        EXPECT_EQ(s.last_report_seq, session.reports_since(0, {}).last_seq);
        std::array<LevelInfo, snapshot_levels> bids{};
        ASSERT_EQ(s.bid_levels, session.book().depth(Side::Buy, bids));
        EXPECT_TRUE(std::equal(bids.begin(), bids.begin() + static_cast<std::ptrdiff_t>(s.bid_levels), s.bids.begin()));
        EXPECT_EQ(s.account.position, session.portfolio().position());
        EXPECT_EQ(s.account.pnl, session.portfolio().pnl());
        EXPECT_EQ(s.my_order_count, session.execution().resting_orders().size());
        // Watermarks never go back.
        EXPECT_GE(s.exchange_ts, previous.exchange_ts);
        EXPECT_GE(s.event_seq, previous.event_seq);
        EXPECT_GE(s.last_report_seq, previous.last_report_seq);
        EXPECT_GE(s.last_command_seq, previous.last_command_seq);
        previous = s;
        ++checked;
    }
    EXPECT_GE(checked, 30u);
    EXPECT_GT(session.execution().statistics().maker_fills, 0u);  // the account and queue fields moved
}

TEST(SnapshotTest, MyOrdersShowTheSharesAndOrdersAheadInTheQueue)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 30), add(110, 2, Side::Buy, 999'900, 20),
                                          add(120, 3, Side::Sell, 1'000'000, 100),
                                          add(1'000, 4, Side::Buy, 999'900, 50),
                                          add(5'000, 5, Side::Sell, 1'000'100, 10)};
    ReplaySession session{config(), over(events)};
    session.control(step(3));
    session.tick(kWall);
    session.submit(post_only_buy(999'900, 10), 0);
    session.tick(kWall);
    session.control(step(1));  // ours arrives at 120 behind orders 1 and 2; order 4 joins behind it
    session.tick(kWall + kSecond);

    const Snapshot s = latest(session);
    ASSERT_EQ(s.my_order_count, 1u);
    const MyOrder& mine = s.my_orders[0];
    EXPECT_EQ(mine.id, 1u);
    EXPECT_EQ(mine.price, Price{999'900});
    EXPECT_EQ(mine.remaining, 10u);
    EXPECT_EQ(mine.ahead_quantity, 50u);
    EXPECT_EQ(mine.ahead_orders, 2u);
    ASSERT_EQ(s.bid_levels, 1u);
    EXPECT_EQ(s.bids[0], (LevelInfo{.price = Price{999'900}, .quantity = 100, .order_count = 3}));
    ASSERT_EQ(s.ask_levels, 1u);
    EXPECT_EQ(s.asks[0], (LevelInfo{.price = Price{1'000'000}, .quantity = 100, .order_count = 1}));
    EXPECT_EQ(s.event_seq, 4u);
    EXPECT_EQ(s.state, SessionState::Paused);
}

TEST(SnapshotTest, TradesAndClosedBarsBeyondTheirRingsAreTruncatedToTheLatest)
{
    std::vector<MarketEvent> events{add(1, 1, Side::Sell, 1'000'000, 1'000)};
    for (Timestamp second = 1; second <= 602; ++second) {
        events.push_back(execute(second * kSecond, 1, 1));  // one trade a second
    }
    ReplaySession session{config(), over(events)};
    session.control(step(601));  // the add and the trades to 600 s
    session.tick(kWall);

    const Snapshot s = latest(session);
    EXPECT_EQ(s.state, SessionState::Paused);
    ASSERT_EQ(s.trade_count, snapshot_trades);
    EXPECT_TRUE(s.trades_truncated);
    EXPECT_EQ(s.trades[0],
              (Trade{.timestamp = 89 * kSecond, .price = Price{1'000'000}, .quantity = 1, .side = Side::Sell}));
    EXPECT_EQ(s.trades[snapshot_trades - 1].timestamp, 600 * kSecond);
    ASSERT_EQ(s.closed_count, snapshot_candles);  // 599 bars closed: seconds 1 to 599
    EXPECT_TRUE(s.candles_truncated);
    EXPECT_EQ(s.closed_candles[0].start, 536 * kSecond);
    EXPECT_EQ(s.closed_candles[snapshot_candles - 1],
              (Candle{.start = 599 * kSecond,
                      .open = Price{1'000'000},
                      .high = Price{1'000'000},
                      .low = Price{1'000'000},
                      .close = Price{1'000'000},
                      .volume = 1}));
    ASSERT_TRUE(s.has_candle);
    EXPECT_EQ(s.candle.start, 600 * kSecond);

    // The next snapshot holds only what came after: two trades and the two bars they closed.
    session.control(step(2));
    session.tick(kWall + snapshot_interval);
    const Snapshot next = latest(session);
    EXPECT_EQ(next.snapshot_seq, 2u);
    EXPECT_EQ(next.state, SessionState::Ended);
    ASSERT_EQ(next.trade_count, 2u);
    EXPECT_FALSE(next.trades_truncated);
    EXPECT_EQ(next.trades[0].timestamp, 601 * kSecond);
    EXPECT_EQ(next.trades[1].timestamp, 602 * kSecond);
    ASSERT_EQ(next.closed_count, 2u);
    EXPECT_FALSE(next.candles_truncated);
    EXPECT_EQ(next.closed_candles[0].start, 600 * kSecond);
    EXPECT_EQ(next.closed_candles[1].start, 601 * kSecond);
    ASSERT_TRUE(next.has_candle);
    EXPECT_EQ(next.candle.start, 602 * kSecond);
}

TEST(SnapshotTest, SnapshotsAreDroppedNotWaitedForWhileAReaderHoldsTheSlot)
{
    std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100)};
    for (Timestamp second = 1; second <= 5; ++second) {
        events.push_back(execute(second * kSecond, 1, 1));  // one trade a second
    }
    ReplaySession session{config(), over(events)};
    session.control(step(3));  // the add and the trades at 1 s and 2 s
    session.tick(kWall);       // the first snapshot: two trades, the bar of 1 s closed

    std::promise<void> holding;
    std::promise<void> release;
    std::future<void> held = holding.get_future();
    std::future<void> released = release.get_future();
    std::thread reader{[&] {
        session.read_snapshot([&](const Snapshot&) {
            holding.set_value();
            released.wait();
        });
    }};
    held.wait();
    session.control(kPause);
    session.control(step(2));       // the trades at 3 s and 4 s
    session.tick(kWall + kSecond);  // something new and due, but the slot is held: dropped, not waited for
    release.set_value();
    reader.join();
    session.control(step(1));  // the trade at 5 s
    session.tick(kWall + 2 * kSecond);

    const Snapshot s = latest(session);
    EXPECT_EQ(s.snapshot_seq, 2u);
    EXPECT_EQ(s.metrics.dropped_snapshots, 1u);
    // The dropped frame's trades and bars reach this one, and the first frame's do not.
    ASSERT_EQ(s.trade_count, 3u);
    EXPECT_FALSE(s.trades_truncated);
    EXPECT_EQ(s.trades[0].timestamp, 3 * kSecond);
    EXPECT_EQ(s.trades[1].timestamp, 4 * kSecond);
    EXPECT_EQ(s.trades[2].timestamp, 5 * kSecond);
    ASSERT_EQ(s.closed_count, 3u);
    EXPECT_FALSE(s.candles_truncated);
    EXPECT_EQ(s.closed_candles[0].start, 2 * kSecond);
    EXPECT_EQ(s.closed_candles[1].start, 3 * kSecond);
    EXPECT_EQ(s.closed_candles[2].start, 4 * kSecond);
    ASSERT_TRUE(s.has_candle);
    EXPECT_EQ(s.candle.start, 5 * kSecond);
}

TEST(SnapshotTest, AFailingStreamEndsTheSessionWithItsFailureInTheSnapshot)
{
    const std::vector<MarketEvent> events{add(200, 1, Side::Buy, 999'900, 100), add(100, 2, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    session.control(kFullSpeed);
    session.control(kPlay);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);

    const Snapshot s = latest(session);
    EXPECT_EQ(s.state, SessionState::Ended);
    EXPECT_EQ(s.failure, ReplayFailure::OutOfOrderTimestamp);
    EXPECT_EQ(s.event_seq, 1u);
    EXPECT_EQ(session.submit(ioc_buy(10), 0).error, ErrorCode::NotReady);
}

TEST(SnapshotTest, APausedSessionPublishesAChangeAtTheNextFrameOnly)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.snapshot_due(), ReplaySession::never);  // the first snapshot is out; nothing is new

    session.submit(cancel(77), 0);
    session.tick(kWall + 10 * kMs);
    EXPECT_EQ(session.snapshot_due(), kWall + snapshot_interval);  // new, but within 1/30 s of the last
    EXPECT_EQ(latest(session).snapshot_seq, 1u);

    session.tick(kWall + snapshot_interval);
    EXPECT_EQ(session.snapshot_due(), ReplaySession::never);
    const Snapshot s = latest(session);
    EXPECT_EQ(s.snapshot_seq, 2u);
    EXPECT_EQ(s.last_command_seq, 1u);
    EXPECT_EQ(s.metrics.command_queue_depth, 0u);  // this tick drained nothing: the last burst does not linger
}

}  // namespace
}  // namespace simulator::server::test
