#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/command_log.hpp"
#include "chronos/server/peer.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using nlohmann::json;

// What one call to the peer sent, parsed.
std::vector<json> parsed(const std::vector<std::string>& out)
{
    std::vector<json> messages;
    for (const std::string& text : out) {
        messages.push_back(json::parse(text));
    }
    return messages;
}

std::vector<json> poll(Peer& peer, Timestamp now = kWall)
{
    std::vector<std::string> out;
    EXPECT_TRUE(peer.poll(now, out));
    return parsed(out);
}

// The peer's replies to `text`; a test failure if it closes the connection.
std::vector<json> send(Peer& peer, const std::string& text, Timestamp now = kWall)
{
    std::vector<std::string> out;
    EXPECT_TRUE(peer.receive(text, now, out)) << text;
    return parsed(out);
}

std::vector<std::string> types(const std::vector<json>& messages)
{
    std::vector<std::string> names;
    for (const json& message : messages) {
        names.push_back(message["type"]);
    }
    return names;
}

const std::string kBuy = R"(,"client_id":9,"side":"buy","order_type":"post_only","price":999800,"qty":10)";

TEST(PeerTest, HelloThenAStateThenOnlyTheLatestSnapshotAndNewReports)
{
    const std::vector<MarketEvent> events = spaced_bids(100);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    Peer peer{hub, kWall};
    EXPECT_EQ(types(poll(peer)), (std::vector<std::string>{"hello"}));  // no snapshot published yet

    session.tick(kWall);
    std::vector<json> first = poll(peer);
    ASSERT_EQ(types(first), (std::vector<std::string>{"state"}));
    EXPECT_EQ(first[0]["full_resync"], false);
    const std::uint64_t last_seq = first[0]["last_report_seq"];
    EXPECT_TRUE(poll(peer).empty());  // nothing new

    // Three snapshots between two polls: the client gets the latest; the reports since the state all arrive.
    session.control(step(1));
    session.tick(kWall + snapshot_interval);
    session.control(step(1));
    session.tick(kWall + 2 * snapshot_interval);
    session.control(step(1));
    session.tick(kWall + 3 * snapshot_interval);
    const std::vector<json> later = poll(peer);
    ASSERT_EQ(types(later), (std::vector<std::string>{"ack", "ack", "ack", "snapshot"}));
    EXPECT_EQ(later[0]["seq"], last_seq + 1);
    EXPECT_EQ(later[3]["snapshot_seq"], 4);  // 2 and 3 were dropped for this client
    EXPECT_EQ(later[3]["event_seq"], 3);
}

TEST(PeerTest, TheSecondConnectionIsAViewerAndGetsNotController)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    auto first = std::make_unique<Peer>(hub, kWall);
    Peer second{hub, kWall};
    EXPECT_EQ(poll(*first).at(0)["role"], "controller");
    EXPECT_EQ(poll(second).at(0)["role"], "viewer");

    for (const std::string& text : {client_message("order", kBuy), client_message("control", R"(,"action":"play")")}) {
        const std::vector<json> reply = send(second, text);
        ASSERT_EQ(reply.size(), 1u);
        EXPECT_EQ(reply[0]["code"], "not_controller");
    }
    // An order's refusal names its client_id; a control's has none to name.
    EXPECT_EQ(send(second, client_message("order", kBuy)).at(0)["client_id"], 9);
    EXPECT_FALSE(send(second, client_message("control", R"(,"action":"play")")).at(0).contains("client_id"));
    EXPECT_TRUE(send(second, client_message("resync", R"(,"from_report_seq":1)")).empty());  // a viewer may resync
    EXPECT_TRUE(send(*first, client_message("order", kBuy)).empty());

    first.reset();  // the controller leaves: the next connection takes control, the viewer stays one
    Peer third{hub, kWall};
    EXPECT_EQ(poll(third).at(0)["role"], "controller");
    EXPECT_EQ(send(second, client_message("order", kBuy)).at(0)["code"], "not_controller");
}

TEST(PeerTest, WithATokenTheFirstMessageMustBeAuthWithinFiveSeconds)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test", .token = "t0k3n"};
    std::vector<std::string> out;

    Peer early{hub, kWall};
    EXPECT_TRUE(early.poll(kWall + Peer::auth_timeout - 1, out));  // nothing is sent before auth
    EXPECT_TRUE(out.empty());
    EXPECT_FALSE(early.poll(kWall + Peer::auth_timeout, out));  // 5 s without auth: close

    Peer chatty{hub, kWall};
    EXPECT_FALSE(chatty.receive(client_message("control", R"(,"action":"play")"), kWall, out));  // not auth first
    Peer wrong{hub, kWall};
    EXPECT_FALSE(wrong.receive(R"({"type":"auth","token":"t0k3m"})", kWall, out));
    Peer garbled{hub, kWall};
    EXPECT_FALSE(garbled.receive("{", kWall, out));
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(session.control(kPause).seq, 1u);  // no control got through: this is the first

    Peer right{hub, kWall};
    EXPECT_TRUE(send(right, R"({"type":"auth","token":"t0k3n"})").empty());
    EXPECT_EQ(poll(right, kWall + Peer::auth_timeout).at(0)["role"], "controller");
    EXPECT_EQ(send(right, R"({"type":"auth","token":"t0k3n"})").at(0)["code"], "bad_request");  // only once

    // The right token is no use at or after the deadline, even before poll() has closed the connection.
    Peer late{hub, kWall};
    EXPECT_FALSE(late.receive(R"({"type":"auth","token":"t0k3n"})", kWall + Peer::auth_timeout, out));
    Peer in_time{hub, kWall};
    EXPECT_TRUE(in_time.receive(R"({"type":"auth","token":"t0k3n"})", kWall + Peer::auth_timeout - 1, out));
    EXPECT_TRUE(out.empty());

    EXPECT_LT(sizeof(Peer), 256u);  // a connection that has not authenticated is cheap: poll()'s scratch is the hub's
}

TEST(PeerTest, AResyncWithinTheRingReplaysTheReportsTheClientMissed)
{
    const std::vector<MarketEvent> events = spaced_bids(100);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    session.tick(kWall);
    std::uint64_t seen = 0;  // the last report seq the first connection got
    {
        Peer before{hub, kWall};
        poll(before);
        session.control(step(1));
        session.tick(kWall);
        for (const json& message : poll(before)) {
            seen = message.contains("seq") ? message["seq"].get<std::uint64_t>() : seen;
        }
    }
    ASSERT_GT(seen, 0u);
    session.control(step(1));  // while it is away
    session.control(step(1));
    session.tick(kWall + snapshot_interval);

    Peer after{hub, kWall};
    EXPECT_EQ(types(poll(after)), (std::vector<std::string>{"hello", "state"}));
    EXPECT_TRUE(send(after, client_message("resync", R"(,"from_report_seq":)" + std::to_string(seen + 1))).empty());
    const std::vector<json> missed = poll(after);
    ASSERT_EQ(types(missed), (std::vector<std::string>{"ack", "ack"}));
    EXPECT_EQ(missed[0]["seq"], seen + 1);
    EXPECT_EQ(missed[1]["seq"], seen + 2);

    // Reports wait for the first state: a resync sent before the first poll still gets hello, state, then the reports.
    Peer early{hub, kWall};
    EXPECT_TRUE(send(early, client_message("resync", R"(,"from_report_seq":)" + std::to_string(seen + 1))).empty());
    EXPECT_EQ(types(poll(early)), (std::vector<std::string>{"hello", "state"}));
    EXPECT_EQ(types(poll(early)), (std::vector<std::string>{"ack", "ack"}));

    // A position is clamped to the ring: 0 is its start, and one past its end waits for the next record.
    const std::uint64_t last = session.reports_since(0, {}).last_seq;
    Peer zero{hub, kWall};
    EXPECT_TRUE(send(zero, client_message("resync", R"(,"from_report_seq":0)")).empty());
    poll(zero);
    const std::vector<json> everything = poll(zero);
    ASSERT_FALSE(everything.empty());
    EXPECT_EQ(everything.front()["seq"], 1);
    EXPECT_EQ(everything.back()["seq"], last);
    Peer future{hub, kWall};
    EXPECT_TRUE(send(future, client_message("resync", R"(,"from_report_seq":1000000)")).empty());
    poll(future);
    EXPECT_TRUE(poll(future).empty());
    session.control(step(1));
    session.tick(kWall + 2 * snapshot_interval);
    EXPECT_EQ(poll(future).at(0)["seq"], last + 1);
}

TEST(PeerTest, AResyncBeyondTheRingGetsAGapThenAFullState)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    session.tick(kWall);
    while (session.reports_since(0, {}).last_seq <= ReplaySession::ring_capacity) {
        session.submit(cancel(1), 0);  // two records each: Received, then Admitted or refused queue_full
    }
    session.tick(kWall + snapshot_interval);  // a snapshot whose watermark is past the overwritten records
    const RecordRing<Record>::Read ring = session.reports_since(0, {});
    ASSERT_GT(ring.first_seq, 1u);

    Peer peer{hub, kWall};
    poll(peer);
    EXPECT_TRUE(send(peer, client_message("resync", R"(,"from_report_seq":1)")).empty());
    const std::vector<json> resync = poll(peer);
    ASSERT_EQ(types(resync), (std::vector<std::string>{"gap", "state"}));
    EXPECT_EQ(resync[0]["from"], 1);
    EXPECT_EQ(resync[0]["to"], ring.first_seq - 1);
    EXPECT_EQ(resync[1]["full_resync"], true);

    // Reports go on after the state's watermark, never repeating what it holds.
    session.control(kPause);
    session.tick(kWall + 2 * snapshot_interval);
    const std::vector<json> next = poll(peer);
    ASSERT_FALSE(next.empty());
    EXPECT_EQ(next[0]["seq"], resync[1]["last_report_seq"].get<std::uint64_t>() + 1);
}

TEST(PeerTest, AStateOlderThanTheRingStartsReportsAtTheRingsFirstRecordOneChunkAtATime)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    session.tick(kWall);  // the only snapshot: its watermark stays at the start of the ring
    while (session.reports_since(0, {}).last_seq <= ReplaySession::ring_capacity + 2 * Peer::reports_per_poll) {
        session.submit(cancel(1), 0);
    }
    const RecordRing<Record>::Read ring = session.reports_since(0, {});

    Peer peer{hub, kWall};
    poll(peer);
    EXPECT_TRUE(send(peer, client_message("resync", R"(,"from_report_seq":1)")).empty());
    const std::vector<json> resync = poll(peer);
    ASSERT_EQ(types(resync), (std::vector<std::string>{"gap", "state"}));
    ASSERT_LT(resync[1]["last_report_seq"].get<std::uint64_t>() + 1, ring.first_seq);  // the state is behind the ring

    // The cursor is the ring's first record, not the state's watermark; then a chunk a poll, each after the last.
    const std::vector<json> chunk = poll(peer);
    ASSERT_EQ(chunk.size(), Peer::reports_per_poll);
    EXPECT_EQ(chunk.front()["seq"], ring.first_seq);
    EXPECT_EQ(chunk.back()["seq"], ring.first_seq + Peer::reports_per_poll - 1);
    EXPECT_EQ(poll(peer).at(0)["seq"], ring.first_seq + Peer::reports_per_poll);
}

TEST(PeerTest, TheSixtyFifthControlBeforeATickIsRefusedAsQueueFull)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    Peer peer{hub, kWall};
    poll(peer);
    const std::string pause = client_message("control", R"(,"action":"pause")");
    for (std::size_t i = 0; i < ReplaySession::control_capacity; ++i) {
        ASSERT_TRUE(send(peer, pause).empty()) << i;
    }
    const std::vector<json> refused = send(peer, pause);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0]["code"], "queue_full");

    session.tick(kWall);  // the engine drains the controls
    EXPECT_TRUE(send(peer, pause).empty());
}

TEST(PeerTest, ACommandIsAckedOnceLoggedAndSubmitsAreRateLimited)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    const Scratch scratch{"peer-ack"};
    LogQueue queue;
    const LogWriter writer{queue, scratch.path, config(), false};
    ReplaySession session{config(), over(events), &queue};
    Hub hub{.session = session, .log = &queue, .writer = &writer, .session_id = "test"};
    Peer peer{hub, kWall};
    poll(peer);

    EXPECT_TRUE(send(peer, client_message("order", kBuy)).empty());
    std::vector<json> acked;
    ASSERT_TRUE(eventually([&] {
        acked = poll(peer);
        return !acked.empty();
    }));
    EXPECT_EQ(acked[0],
              json::parse(R"({"type":"ack","session_id":"test","generation":1,"command_seq":1,)"
                          R"("persisted":"flushed"})"));

    // A refusal at admission names its command; a stale generation is the engine's refusal.
    const std::vector<json> stale = send(peer, client_message("order", kBuy, 2));
    EXPECT_EQ(stale.at(0)["code"], "stale_generation");
    EXPECT_EQ(stale.at(0)["command_seq"], 2);
    EXPECT_EQ(stale.at(0)["client_id"], 9);

    // 50 submits a second: the 51st in the same second is refused, the next second admits again.
    for (int i = 2; i < 50; ++i) {
        ASSERT_TRUE(send(peer, client_message("cancel", R"(,"order_id":1)")).empty());
    }
    EXPECT_EQ(send(peer, client_message("cancel", R"(,"order_id":1)")).at(0)["code"], "rate_limited");
    const std::vector<json> limited = send(peer, client_message("order", kBuy));
    EXPECT_EQ(limited.at(0)["code"], "rate_limited");
    EXPECT_EQ(limited.at(0)["client_id"], 9);
    EXPECT_TRUE(send(peer, client_message("cancel", R"(,"order_id":1)"), kWall + kSecond).empty());
}

TEST(PeerTest, ACommandWhoseRecordNeverReachesTheLogGetsLogUnavailable)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    const Scratch scratch{"peer-unavailable"};
    LogQueue queue;  // the session's: no writer drains it
    LogQueue idle;
    const LogWriter writer{idle, scratch.path, config(), false};  // persists nothing of this session's
    ReplaySession session{config(), over(events), &queue};
    Hub hub{.session = session, .log = &queue, .writer = &writer, .session_id = "test"};
    Peer peer{hub, kWall};
    poll(peer);
    EXPECT_TRUE(send(peer, client_message("order", kBuy)).empty());
    EXPECT_TRUE(poll(peer).empty());  // not yet logged: no ack

    queue.fail();
    const std::vector<json> refused = poll(peer);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0]["code"], "log_unavailable");
    EXPECT_EQ(refused[0]["command_seq"], 1);
    EXPECT_EQ(refused[0]["client_id"], 9);
}

TEST(PeerTest, ACommandLoggedBeforeTheLogFailedIsStillAcked)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    const Scratch scratch{"peer-failed-after"};
    LogQueue queue;
    const LogWriter writer{queue, scratch.path, config(), false};
    ReplaySession session{config(), over(events), &queue};
    Hub hub{.session = session, .log = &queue, .writer = &writer, .session_id = "test"};
    Peer peer{hub, kWall};
    poll(peer);
    EXPECT_TRUE(send(peer, client_message("order", kBuy)).empty());
    const std::uint64_t admitted = session.reports_since(0, {}).last_seq;  // the order's Admitted record
    ASSERT_TRUE(eventually([&] { return writer.persisted_seq() >= admitted; }));

    queue.fail();
    const std::vector<json> reply = poll(peer);
    ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply[0]["type"], "ack");
    EXPECT_EQ(reply[0]["command_seq"], 1);
}

TEST(PeerTest, MessagesForAnotherSessionOrGenerationAndMalformedOnesAreRefused)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test", .symbols = {"AAPL"}};
    Peer peer{hub, kWall};
    poll(peer);
    const auto code = [&](const std::string& text) {
        const std::vector<json> reply = send(peer, text);
        return reply.empty() ? std::string{} : reply[0]["code"].get<std::string>();
    };
    EXPECT_EQ(code("not json"), "bad_request");
    EXPECT_FALSE(send(peer, "not json").at(0).contains("client_id"));
    // An order refused after its client_id was read names it, even when another key is what failed.
    const std::vector<json> no_price = send(peer,
                                            client_message("order",
                                                           R"(,"client_id":9,"side":"buy",)"
                                                           R"("order_type":"ioc","qty":1)"));
    EXPECT_EQ(no_price.at(0)["code"], "bad_request");
    EXPECT_EQ(no_price.at(0)["client_id"], 9);
    EXPECT_EQ(code(R"({"type":"control","session_id":"old","generation":1,"action":"play"})"), "stale_generation");
    EXPECT_EQ(code(client_message("control", R"(,"action":"play")", 2)), "stale_generation");
    EXPECT_EQ(code(client_message("control", R"(,"action":"symbol","symbol":"MSFT")")), "bad_request");
    EXPECT_EQ(code(client_message("control", R"(,"action":"symbol","symbol":"AAPL")")), "");
    EXPECT_EQ(code(client_message("control", R"(,"action":"speed","speed":1000)")), "");
}

TEST(PeerTest, ASeekSendsProgressAndTheNewGenerationArrivesAsAState)
{
    const std::vector<MarketEvent> events = spaced_bids(10'000);
    ReplaySession session{config(), over(events)};
    Hub hub{.session = session, .session_id = "test"};
    Peer peer{hub, kWall};
    session.control(step(9'000));
    session.tick(kWall);
    session.tick(kWall);
    session.tick(kWall);
    poll(peer);

    EXPECT_TRUE(send(peer, client_message("control", R"(,"action":"seek","ts":6000000000)")).empty());  // backward
    session.tick(kWall + snapshot_interval);  // restarts, then seeks part of the way
    const std::vector<json> seeking = poll(peer);
    ASSERT_FALSE(seeking.empty());
    EXPECT_EQ(seeking.back()["type"], "progress");
    EXPECT_EQ(seeking.back()["generation"], 2);
    EXPECT_EQ(seeking.back()["seek"], 4'096 * kMs);

    session.tick(kWall + 2 * snapshot_interval);  // the seek ends, and publishes generation 2's first snapshot
    const std::vector<json> arrived = poll(peer);
    ASSERT_EQ(arrived.back()["type"], "state");
    EXPECT_EQ(arrived.back()["generation"], 2);
    EXPECT_EQ(arrived.back()["event_seq"], 6'000);
    EXPECT_EQ(send(peer, client_message("order", kBuy)).at(0)["code"], "stale_generation");  // generation 1's
}

}  // namespace
}  // namespace simulator::server::test
