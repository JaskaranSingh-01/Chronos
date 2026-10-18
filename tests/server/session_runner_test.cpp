#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "chronos/server/session_runner.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

TEST(SessionRunnerTest, RunsASessionToTheEndAtFullSpeed)
{
    const std::vector<MarketEvent> events = random_stream(20'000);
    ReplaySession session{config(), over(events)};
    const auto start = std::chrono::steady_clock::now();
    {
        const SessionRunner runner{session};
        session.control(kFullSpeed);
        session.control(kPlay);
        ASSERT_TRUE(eventually([&] {
            SessionState published = SessionState::Paused;
            session.read_snapshot([&](const Snapshot& s) { published = s.state; });
            return published == SessionState::Ended;
        }));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
    EXPECT_EQ(session.state(), SessionState::Ended);
    EXPECT_EQ(session.loop_report().replay.events, 20'000u);
    EXPECT_EQ(latest(session).event_seq, 20'000u);
}

TEST(SessionRunnerTest, ACommandWakesAPausedSession)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100),
                                          add(1'000, 2, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    const SessionRunner runner{session};
    ASSERT_TRUE(eventually([&] { return session.read_snapshot([](const Snapshot&) {}); }));
    std::this_thread::sleep_for(std::chrono::milliseconds{5});  // the runner is parked with no deadline
    ASSERT_EQ(session.submit(post_only_buy(999'800, 10), 0).error, ErrorCode::None);
    EXPECT_TRUE(eventually([&] { return trace(session) == std::vector<std::string>{"applied cmd1 @99 ev0"}; }));
}

// The runner reads snapshot_due() after the tick that drained the command, else it parks with the change unpublished.
TEST(SessionRunnerTest, ACommandRightAfterASnapshotReachesTheNextSnapshot)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    const SessionRunner runner{session};
    ASSERT_TRUE(eventually([&] { return session.read_snapshot([](const Snapshot&) {}); }));
    ASSERT_EQ(session.submit(cancel(77), 0).error, ErrorCode::None);  // within 1/30 s of that snapshot
    EXPECT_TRUE(eventually([&] {
        std::uint64_t last_command_seq = 0;
        session.read_snapshot([&](const Snapshot& s) { last_command_seq = s.last_command_seq; });
        return last_command_seq == 1;
    }));
}

TEST(SessionRunnerTest, APlayingSessionPublishesSnapshotsThroughAQuietStretch)
{
    const std::vector<MarketEvent> events{add(kMs, 1, Side::Buy, 999'900, 100),
                                          add(100 * kSecond, 2, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    const SessionRunner runner{session};
    session.control(kPlay);
    struct Mark
    {
        SessionState state{SessionState::Paused};
        std::uint64_t seq{0};
        Timestamp exchange_ts{0};
    };
    const auto mark = [&] {
        Mark m;
        session.read_snapshot([&](const Snapshot& s) { m = {s.state, s.snapshot_seq, s.exchange_ts}; });
        return m;
    };
    Mark first;
    ASSERT_TRUE(eventually([&] { return (first = mark()).state == SessionState::Playing; }));
    Mark last;
    // No market event for 100 s, yet frames keep coming; their rate is the OS scheduler's (macOS CI coalesces timers).
    ASSERT_TRUE(eventually([&] { return (last = mark()).seq >= first.seq + 15; }));
    EXPECT_GE(last.exchange_ts - first.exchange_ts, 450 * kMs);  // at 1x, exchange time follows
}

TEST(SessionRunnerTest, PlaysInRealTimeAtOneTimesSpeed)
{
    const std::vector<MarketEvent> events{add(kSecond, 1, Side::Buy, 999'900, 100),
                                          add(kSecond + 300 * kMs, 2, Side::Sell, 1'000'000, 100),
                                          add(100 * kSecond, 3, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    const SessionRunner runner{session};
    const auto start = std::chrono::steady_clock::now();
    session.control(kPlay);
    ASSERT_TRUE(eventually([&] {
        std::uint64_t applied = 0;
        session.read_snapshot([&](const Snapshot& s) { applied = s.event_seq; });
        return applied == 2;
    }));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_GE(elapsed, std::chrono::milliseconds{300});  // the second event is 300 ms of exchange time later
    EXPECT_LT(elapsed, std::chrono::seconds{3});
}

TEST(SessionRunnerTest, StoppingAWaitingRunnerReturnsPromptly)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100)};
    ReplaySession session{config(), over(events)};
    const auto start = std::chrono::steady_clock::now();
    {
        const SessionRunner runner{session};  // paused: it waits with no deadline
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{1});
}

}  // namespace
}  // namespace simulator::server::test
