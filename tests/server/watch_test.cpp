#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/command_log.hpp"
#include "chronos/server/lab.hpp"
#include "chronos/server/peer.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;
using nlohmann::json;

SessionConfig lab_config()
{
    SessionConfig cfg{.session_id = "test", .instrument = kEquity};
    cfg.defaults.days = {.day = {2019y / 12 / 30}, .count = 1};
    return cfg;
}

// Friday's and Monday's ACME, the Lab that runs over them, and a terminal session over the same library whose Watch
// controls come through a Peer, as the controller's would.
struct Terminal
{
    ScratchDir dir;
    DayLibrary library{dir.path / "data", dir.path / "fingerprints.json"};
    Backtester lab{lab_config(), dir.path / "runs"};
    LogQueue queue;
    LogWriter writer{queue, dir.path / "log.jsonl", lab_config(), &library, false};
    ReplaySession session{lab_config(), chain_factory(library, kEquity, false), &queue,
                          chain_factory(library, kEquity, false, true)};
    Hub hub{.session = session, .session_id = "test", .library = &library, .lab = &lab};
    Peer peer{hub, kWall};
    Timestamp wall{kWall};

    explicit Terminal(const std::string& name)
        : dir(name)
    {
        std::filesystem::create_directories(dir.path / "data");
        write_file(dir.path / "data" / "12272019.NASDAQ_ITCH50.bin", lab_day(1).bytes());
        write_file(dir.path / "data" / "12302019.NASDAQ_ITCH50.bin", lab_day(2).bytes());
        library.scan();
    }

    // A completed Lab run of `kind` on ACME over Friday, or over Friday and Monday.
    std::string run(SessionStrategyKind kind, bool two_days, TradingWindow window)
    {
        RunSpec spec = default_spec(kind);
        spec.ticker = ticker("ACME");
        spec.days = two_days ? DayList{.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2}
                             : DayList{.day = {2019y / 12 / 27}, .count = 1};
        spec.window = window;
        std::vector<DayHandle> days;
        for (std::size_t i = 0; i < spec.days.count; ++i) {
            days.push_back(library.find(spec.days.day[i]));
        }
        std::string id;
        EXPECT_EQ(lab.submit(spec, days, id), Refusal::None);
        EXPECT_TRUE(eventually([&] {
            std::string text;
            return lab.get(id, text) == Refusal::None && json::parse(text)["state"] == "completed";
        }));
        return id;
    }

    // The controller's Watch of `id`: the error message it gets, or "" once the next tick has started it.
    std::string watch(const std::string& id)
    {
        std::vector<std::string> out;
        EXPECT_TRUE(peer.receive(
            client_message("control", R"(,"action":"watch","run_id":")" + id + "\"", session.generation()), wall, out));
        if (!out.empty()) {
            return json::parse(out[0])["message"];
        }
        session.tick(wall += snapshot_interval);
        return "";
    }

    void tick()
    {
        session.tick(wall += snapshot_interval);
    }

    void play_to_end()
    {
        session.control(kFullSpeed);
        session.control(kPlay);
        while (session.state() != SessionState::Ended) {
            tick();
        }
    }

    // The verdict of the current generation; a test failure if it has none.
    Record verdict()
    {
        const std::vector<Record> all = records(session);
        for (auto r = all.rbegin(); r != all.rend(); ++r) {
            if (r->kind == RecordKind::Verdict && r->generation == session.generation()) {
                return *r;
            }
        }
        ADD_FAILURE() << "no verdict for generation " << session.generation();
        return {};
    }
};

// Spec §8, equality 2: Watch replays each Lab run, both strategies over one and two days, under a Regular and a Full
// day window, to the run's digests. One plays in steps, at a speed, paused and seeking; one restarts part-way, and its
// new generation, the same Watch, re-hashes and gets its own verdict.
TEST(WatchTest, EveryRunReproducesInTheTerminalAtAnyPace)
{
    Terminal t{"watch-runs"};
    int n = 0;
    for (const SessionStrategyKind kind : {SessionStrategyKind::MarketMaker, SessionStrategyKind::ImbalanceTaker}) {
        for (const bool two_days : {false, true}) {
            for (const TradingWindow window : {TradingWindow::Regular, TradingWindow::FullDay}) {
                SCOPED_TRACE(n);
                const std::string id = t.run(kind, two_days, window);
                ASSERT_EQ(t.watch(id), "");
                EXPECT_EQ(name(latest(t.session).run_id), id);
                EXPECT_EQ(latest(t.session).spec.window, window);
                if (n == 0) {
                    t.session.control(step(5));
                    t.tick();
                    t.session.control(Control{.type = ControlType::Speed, .speed = 1'000});
                    t.session.control(kPlay);
                    for (int i = 0; i < 3; ++i) {
                        t.tick();
                    }
                    t.session.control(kPause);
                    t.session.control(Control{.type = ControlType::Seek,
                                              .seek_ts = session_midnight_ns(2019y / 12 / 27, -5h) +
                                                  static_cast<Timestamp>(kNineThirty + 10 * kSecond)});
                    while (t.session.state() != SessionState::Paused) {
                        t.tick();
                    }
                } else if (n == 1) {
                    t.session.control(step(5));
                    t.tick();
                    t.session.control(Control{.type = ControlType::Restart});
                    t.tick();
                    EXPECT_EQ(name(latest(t.session).run_id), id);  // the same Watch
                }
                t.play_to_end();
                const Record verdict = t.verdict();
                EXPECT_EQ(verdict.outcome, Outcome::Reproduced);
                EXPECT_EQ(verdict.digests, verdict.control.watch.expected);
                EXPECT_EQ(name(verdict.control.watch.run_id), id);
                ++n;
            }
        }
    }
}

// A detach during a Watch makes it another run: mismatch. The log's header of a Watch's generation holds the run's
// spec and id, and reproducing that log gives the run's digests again.
TEST(WatchTest, ADetachGivesMismatchAndTheWatchsLogReproducesTheRun)
{
    Terminal t{"watch-detach"};
    const std::string id = t.run(SessionStrategyKind::MarketMaker, true, TradingWindow::Regular);
    ASSERT_EQ(t.watch(id), "");
    const std::uint64_t watched = t.session.generation();
    t.play_to_end();
    EXPECT_EQ(t.verdict().outcome, Outcome::Reproduced);

    ASSERT_EQ(t.watch(id), "");
    t.session.control(step(20));
    t.tick();
    t.session.submit(Command{.generation = t.session.generation(), .type = CommandType::Detach}, 0);
    t.play_to_end();
    EXPECT_EQ(t.verdict().outcome, Outcome::Mismatch);
    t.session.close();
    ASSERT_TRUE(eventually([&] { return t.queue.size() == 0; }));

    LoggedGeneration logged;
    std::string error;
    ASSERT_TRUE(read_log(t.dir.path / "log.jsonl", watched, logged, error)) << error;
    std::ifstream in{t.dir.path / "log.jsonl"};
    for (std::string line; std::getline(in, line);) {
        const json j = json::parse(line);
        if (j["type"] == "header" && j["generation"] == watched) {
            EXPECT_EQ(j["run_id"], id);
            EXPECT_EQ(j["window"], "regular");
            EXPECT_EQ(j["strategy"]["kind"], "market_maker");
        }
    }
    EXPECT_EQ(logged.spec.days, (DayList{.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2}));
    EXPECT_EQ(logged.spec.strategy, default_spec(SessionStrategyKind::MarketMaker).strategy);
    std::vector<DayHandle> days{t.library.find(2019y / 12 / 27), t.library.find(2019y / 12 / 30)};
    Generation again{lab_config(), logged.spec,
                     chain_factory(t.library, kEquity, false)(logged.spec.ticker, logged.spec.days)};
    ASSERT_TRUE(reproduce(again, logged, days));
    Control resolved{.watch = {}};
    std::string why;
    ASSERT_TRUE(to_padded(id, resolved.watch.run_id));
    ASSERT_TRUE(t.lab.watch(resolved, t.library, why)) << why;
    EXPECT_EQ(digests(again), resolved.watch.expected);
}

// A symbol loaded after a Watch trades by hand on the terminal's own settings, not the watched run's strategy, window
// and limits.
TEST(WatchTest, ASymbolLoadedAfterAWatchHasTheTerminalsOwnSettings)
{
    Terminal t{"watch-then-symbol"};
    ASSERT_EQ(t.watch(t.run(SessionStrategyKind::MarketMaker, false, TradingWindow::Regular)), "");
    t.session.control(Control{.type = ControlType::Symbol,
                              .spec = {.ticker = ticker("ACME"), .days = {.day = {2019y / 12 / 30}, .count = 1}}});
    t.tick();
    const Snapshot now = latest(t.session);
    EXPECT_EQ(name(now.run_id), "");
    EXPECT_EQ(now.spec.strategy.kind, SessionStrategyKind::None);
    EXPECT_EQ(now.spec.window, lab_config().defaults.window);
    EXPECT_EQ(now.spec.limits.max_order_quantity, lab_config().defaults.limits.max_order_quantity);
}

// A Watch is admitted for a completed run whose days the library lists with the run's hashes, run with this server's
// allow_truncated_tail; else the controller gets bad_request, with the reason, and nothing is loaded.
TEST(WatchTest, AWatchIsAdmittedOnlyForACompletedRunOverTheSameDays)
{
    Terminal t{"watch-admission"};
    const std::string done = t.run(SessionStrategyKind::ImbalanceTaker, false, TradingWindow::Regular);
    const std::uint64_t generation = t.session.generation();
    EXPECT_EQ(t.watch("20261018T101500Z-0000000000000000"), "20261018T101500Z-0000000000000000: no such run");
    std::vector<std::string> out;
    EXPECT_TRUE(t.peer.receive(client_message("control", R"(,"action":"watch")", generation), t.wall, out));
    EXPECT_EQ(json::parse(out.at(0))["code"], "bad_request");
    Peer viewer{t.hub, kWall};
    out.clear();
    EXPECT_TRUE(viewer.receive(client_message("control", R"(,"action":"watch","run_id":")" + done + "\"", generation),
                               t.wall, out));
    EXPECT_EQ(json::parse(out.at(0))["code"], "not_controller");

    std::ofstream{t.dir.path / "data" / "12272019.NASDAQ_ITCH50.bin", std::ios::app | std::ios::binary} << 'x';
    t.library.scan();
    EXPECT_EQ(t.watch(done), "2019-12-27: not the day the run replayed");
    EXPECT_EQ(t.session.generation(), generation);

    // The same run as saved, then as a partial run and as one run with --allow-truncated-tail.
    const std::filesystem::path file = t.dir.path / "runs" / (done + ".json");
    std::ifstream in{file};
    const json saved = json::parse(in);
    const auto refusal = [&](const json& edited) {
        std::ofstream{file} << edited.dump();
        const Backtester reloaded{lab_config(), t.dir.path / "runs"};
        Control control;
        EXPECT_TRUE(to_padded(done, control.watch.run_id));
        std::string why;
        EXPECT_FALSE(reloaded.watch(control, t.library, why));
        return why;
    };
    json partial = saved;
    partial["state"] = "failed";
    partial["results"]["partial"] = true;
    partial["results"]["digests"] = nullptr;
    EXPECT_EQ(refusal(partial), done + ": not a completed run");
    json truncated = saved;
    truncated["provenance"]["allow_truncated_tail"] = true;
    EXPECT_EQ(refusal(truncated), done + ": run with another --allow-truncated-tail");
}

// Spec §4: a Watch reads each segment afresh. Friday's segment damaged in the store since the run fails its read, so
// the replay stops with a store error before Monday, and the verdict is input_changed, never a mismatch.
TEST(WatchTest, AWatchOfADamagedSegmentIsInputChanged)
{
    Terminal t{"watch-damaged"};
    const std::string id = t.run(SessionStrategyKind::MarketMaker, true, TradingWindow::Regular);
    const Segment friday = *t.library.find(2019y / 12 / 27)->segment("ACME");
    flip(t.dir.path / "data" / "store" / "ACME.itch.zst", friday.offset + friday.compressed - 1);  // its checksum

    ASSERT_EQ(t.watch(id), "");
    t.play_to_end();
    const Record verdict = t.verdict();
    EXPECT_EQ(verdict.outcome, Outcome::InputChanged);
    EXPECT_EQ(t.session.loop_report().replay.source_error, SourceError::StoreRead);
}

}  // namespace
}  // namespace simulator::server::test
