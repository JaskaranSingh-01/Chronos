#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "chronos/server/replay_session.hpp"
#include "itch/itch_test_builder.hpp"
#include "simulator/itch/itch_source.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;

// The generations that started, from the ring.
std::vector<std::uint64_t> starts(const ReplaySession& session)
{
    std::vector<std::uint64_t> generations;
    for (const Record& r : records(session)) {
        if (r.kind == RecordKind::GenerationStart) {
            generations.push_back(r.generation);
        }
    }
    return generations;
}

TEST(RestartTest, RestartRefusesStaleCommandsAndStartsAFreshGeneration)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100), add(200, 2, Side::Buy, 999'900, 100),
                                          add(1'000, 3, Side::Buy, 999'800, 100)};
    ReplaySession session{config(), over(events)};
    session.control(step(2));
    session.tick(kWall);
    session.submit(post_only_buy(999'800, 10), 0);
    session.tick(kWall);                            // applied in generation 1
    session.submit(post_only_buy(999'700, 10), 0);  // still queued when the restart comes
    session.control(Control{.type = ControlType::Restart});
    session.tick(kWall);

    EXPECT_EQ(session.generation(), 2u);
    EXPECT_EQ(session.state(), SessionState::Paused);
    EXPECT_EQ(session.loop_report().replay.events, 0u);
    EXPECT_EQ(session.submit(post_only_buy(999'800, 10), 0).error, ErrorCode::StaleGeneration);
    Command fresh = post_only_buy(999'800, 10);
    fresh.generation = 2;
    ASSERT_EQ(session.submit(fresh, 0).error, ErrorCode::None);
    session.tick(kWall);

    EXPECT_EQ(
        trace(session),
        (std::vector<std::string>{"applied cmd1 @200 ev2", "refused cmd2 stale_generation", "applied cmd4 @99 ev0"}));
    EXPECT_EQ(session.portfolio().open_orders(), 1u);  // generation 2 holds only its own order
    EXPECT_EQ(starts(session), (std::vector<std::uint64_t>{1, 2}));
}

TEST(RestartTest, RestartKeepsTheAttachedStrategy)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Buy, 999'900, 100),
                                          add(200, 2, Side::Sell, 1'000'000, 100)};
    ReplaySession session{config(), over(events)};
    session.submit(
        Command{.generation = 1, .type = CommandType::Attach, .strategy = {.kind = SessionStrategyKind::MarketMaker}},
        0);
    session.tick(kWall);
    session.control(Control{.type = ControlType::Restart});
    session.tick(kWall);

    const std::vector<Record> all = records(session);
    const auto start = std::find_if(all.begin(), all.end(), [](const Record& r) {
        return r.kind == RecordKind::GenerationStart && r.generation == 2;
    });
    ASSERT_NE(start, all.end());
    EXPECT_EQ(start->command.strategy.kind, SessionStrategyKind::MarketMaker);
    Command manual = post_only_buy(999'800, 10);
    manual.generation = 2;
    session.submit(manual, 0);
    session.tick(kWall);
    EXPECT_EQ(trace(session).back(), "refused cmd2 not_ready");  // the ticket stays off
}

TEST(RestartTest, SeekRunsInBoundedTicksRefusesCommandsAndStopsAtTheTarget)
{
    const std::vector<MarketEvent> events = spaced_bids(10'000);
    ReplaySession session{config(), over(events)};
    session.control(Control{.type = ControlType::Seek, .seek_ts = 9'000 * kMs});

    EXPECT_EQ(session.tick(kWall), kWall);
    EXPECT_EQ(session.state(), SessionState::Seeking);
    EXPECT_EQ(session.loop_report().replay.events, ReplaySession::events_per_tick);
    EXPECT_EQ(session.seek_position(), 4'096 * kMs);
    EXPECT_FALSE(session.read_snapshot([](const Snapshot&) {}));  // progress only, no snapshot, while seeking
    EXPECT_EQ(session.submit(cancel(7), 0).error, ErrorCode::NotReady);

    EXPECT_EQ(session.tick(kWall), kWall);
    EXPECT_EQ(session.loop_report().replay.events, 2 * ReplaySession::events_per_tick);
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.state(), SessionState::Paused);
    EXPECT_EQ(session.loop_report().replay.events, 9'000u);
    EXPECT_EQ(session.clock(), 9'000 * kMs);
    EXPECT_EQ(session.seek_position(), 9'000 * kMs);
    EXPECT_EQ(latest(session).event_seq, 9'000u);
}

TEST(RestartTest, CancelSeekPausesWhereTheSeekHasGot)
{
    const std::vector<MarketEvent> events = spaced_bids(10'000);
    ReplaySession session{config(), over(events)};
    session.control(Control{.type = ControlType::Seek, .seek_ts = 9'000 * kMs});
    session.tick(kWall);
    session.control(Control{.type = ControlType::CancelSeek});
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.state(), SessionState::Paused);
    EXPECT_EQ(session.loop_report().replay.events, ReplaySession::events_per_tick);
    EXPECT_EQ(session.clock(), 4'096 * kMs);
}

TEST(RestartTest, SeekingBackwardRestartsAndFastForwards)
{
    const std::vector<MarketEvent> events = spaced_bids(100);
    ReplaySession session{config(), over(events)};
    session.control(step(50));
    session.tick(kWall);
    session.control(Control{.type = ControlType::Seek, .seek_ts = 20 * kMs});
    EXPECT_EQ(session.tick(kWall), ReplaySession::never);
    EXPECT_EQ(session.generation(), 2u);
    EXPECT_EQ(session.state(), SessionState::Paused);
    EXPECT_EQ(session.loop_report().replay.events, 20u);
    EXPECT_EQ(session.clock(), 20 * kMs);

    // A generation 1 command queued across a backward seek of two ticks is stale, in the queue and while Seeking.
    const std::vector<MarketEvent> longer = spaced_bids(10'000);
    ReplaySession far{config(), over(longer)};
    far.control(Control{.type = ControlType::Seek, .seek_ts = 6'000 * kMs});
    far.tick(kWall);
    far.tick(kWall);
    ASSERT_EQ(far.clock(), 6'000 * kMs);
    ASSERT_EQ(far.submit(cancel(7), 0).error, ErrorCode::None);
    far.control(Control{.type = ControlType::Seek, .seek_ts = 5'000 * kMs});
    EXPECT_EQ(far.tick(kWall), kWall);
    EXPECT_EQ(far.generation(), 2u);
    EXPECT_EQ(far.state(), SessionState::Seeking);
    EXPECT_EQ(trace(far), (std::vector<std::string>{"refused cmd1 stale_generation"}));
    EXPECT_EQ(far.submit(cancel(7), 0).error, ErrorCode::StaleGeneration);
    Command current = cancel(7);
    current.generation = 2;
    EXPECT_EQ(far.submit(current, 0).error, ErrorCode::NotReady);
    EXPECT_EQ(far.tick(kWall), ReplaySession::never);
    EXPECT_EQ(far.state(), SessionState::Paused);
    EXPECT_EQ(far.clock(), 5'000 * kMs);

    // A seek past the end of the stream ends the session.
    far.control(Control{.type = ControlType::Seek, .seek_ts = 20'000 * kMs});
    far.tick(kWall);
    far.tick(kWall);
    EXPECT_EQ(far.state(), SessionState::Ended);
    EXPECT_EQ(far.loop_report().replay.events, 10'000u);
}

TEST(RestartTest, ASymbolSwitchStartsAGenerationOnTheNewTicker)
{
    const std::vector<MarketEvent> aaa{add(100, 1, Side::Buy, 999'900, 100)};
    const std::vector<MarketEvent> bbb{add(200, 1, Side::Sell, 1'000'100, 50), add(300, 2, Side::Sell, 1'000'200, 50)};
    std::vector<std::string> built;  // the tickers the factory was asked for
    ReplaySession session{config(), [&](std::string_view t) {
                              built.emplace_back(t);
                              return std::make_unique<VectorSource>(t == "BBB" ? bbb : aaa);
                          }};
    EXPECT_EQ(session.control(Control{.type = ControlType::Symbol}).error, ErrorCode::BadRequest);  // no ticker
    session.control(Control{.type = ControlType::Symbol, .symbol = ticker("BBB")});
    session.tick(kWall);

    EXPECT_EQ(session.generation(), 2u);
    EXPECT_EQ(session.state(), SessionState::Paused);
    EXPECT_EQ(name(latest(session).symbol), "BBB");
    session.control(step(2));
    session.tick(kWall);
    EXPECT_EQ(session.loop_report().replay.events, 2u);  // BBB's two events, not the first generation's one

    session.control(Control{.type = ControlType::Restart});  // a restart keeps the ticker
    session.tick(kWall);
    EXPECT_EQ(built, (std::vector<std::string>{"", "BBB", "BBB"}));
    std::vector<std::string> started;  // each generation's ticker, from its GenerationStart record
    for (const Record& r : records(session)) {
        if (r.kind == RecordKind::GenerationStart) {
            started.emplace_back(name(r.control.symbol));
        }
    }
    EXPECT_EQ(started, (std::vector<std::string>{"", "BBB", "BBB"}));
}

TEST(RestartTest, ARestartedItchSessionReplaysTheSameDay)
{
    itch_test::ItchBuilder itch;
    itch.stock_directory(1, 1, "AAPL")
        .stock_directory(2, 2, "MSFT")
        .add_order(1, 1'000, 1, 'B', 100, "AAPL", 2'900'000)
        .add_order(2, 1'500, 9, 'B', 100, "MSFT", 1'500'000)  // another symbol: filtered out
        .add_order(1, 2'000, 2, 'S', 100, "AAPL", 2'900'100)
        .executed(1, 3'000, 2, 40, 1)
        .executed_with_price(1, 1'000'003'000, 2, 10, 2, true, 2'900'050)
        .erase(1, 2'000'000'000, 1);
    const std::span<const std::byte> bytes = itch.bytes();
    const ItchSourceConfig itch_config{.ticker = "AAPL", .session_date = 2019y / 12 / 30, .utc_offset = -5h};
    InstrumentRegistry registry{1};
    SessionConfig cfg = config();
    cfg.source_size = bytes.size();
    ReplaySession session{cfg,
                          [&](std::string_view) { return std::make_unique<ItchSource>(bytes, itch_config, registry); }};
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 30, -5h);

    session.control(kFullSpeed);
    session.control(kPlay);
    session.tick(kWall);
    const Snapshot first = latest(session);
    const std::uint64_t digest = session.book().digest();
    EXPECT_EQ(first.event_seq, 5u);
    EXPECT_EQ(first.progress, 1.0);
    ASSERT_EQ(first.closed_count, 1u);
    EXPECT_EQ(first.closed_candles[0],
              (Candle{.start = midnight,
                      .open = Price{2'900'100},
                      .high = Price{2'900'100},
                      .low = Price{2'900'100},
                      .close = Price{2'900'100},
                      .volume = 40}));
    EXPECT_EQ(first.candle,
              (Candle{.start = midnight + kSecond,
                      .open = Price{2'900'050},
                      .high = Price{2'900'050},
                      .low = Price{2'900'050},
                      .close = Price{2'900'050},
                      .volume = 10}));

    session.control(Control{.type = ControlType::Restart});
    session.tick(kWall + kSecond);
    session.control(kPlay);
    session.tick(kWall + 2 * kSecond);
    const Snapshot second = latest(session);
    EXPECT_EQ(second.generation, 2u);
    EXPECT_EQ(second.event_seq, 5u);
    ASSERT_EQ(second.closed_count, 1u);
    EXPECT_EQ(second.closed_candles[0], first.closed_candles[0]);
    EXPECT_EQ(second.candle, first.candle);
    EXPECT_EQ(session.book().digest(), digest);
    EXPECT_EQ(registry.size(), 1u);  // the second source found AAPL already registered
}

}  // namespace
}  // namespace simulator::server::test
