#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/protocol.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using nlohmann::json;

// `text` decoded; a test failure, with the reason, if it is refused.
ClientMessage decoded(const std::string& text)
{
    ClientMessage decoded;
    std::string error;
    EXPECT_TRUE(decode(text, decoded, error)) << text << ": " << error;
    return decoded;
}

// True if decode() refuses `text`.
bool refused(const std::string& text)
{
    ClientMessage ignored;
    std::string error;
    return !decode(text, ignored, error);
}

TEST(ProtocolTest, DecodesEveryClientMessage)
{
    EXPECT_EQ(decoded(R"({"type":"auth","token":"t0k3n"})").token, "t0k3n");

    const ClientMessage play = decoded(client_message("control", R"(,"action":"play")"));
    EXPECT_EQ(play.type, ClientType::Control);
    EXPECT_EQ(play.session_id, "test");
    EXPECT_EQ(play.generation, 1u);
    EXPECT_EQ(play.control.type, ControlType::Play);
    EXPECT_EQ(decoded(client_message("control", R"(,"action":"step","n":5)")).control.steps, 5u);
    EXPECT_EQ(decoded(client_message("control", R"(,"action":"speed","speed":0)")).control.speed, 0u);
    EXPECT_EQ(decoded(client_message("control", R"(,"action":"seek","ts":1577716200000000123)")).control.seek_ts,
              1'577'716'200'000'000'123);
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"AAPL")")).control.symbol), "AAPL");
    EXPECT_EQ(decoded(client_message("control", R"(,"action":"cancel_seek")")).control.type, ControlType::CancelSeek);

    const ClientMessage ioc = decoded(
        client_message("order", R"(,"client_id":7,"side":"sell","order_type":"ioc","price":2905500,"qty":100)"));
    EXPECT_EQ(ioc.type, ClientType::Command);
    EXPECT_EQ(ioc.command,
              (Command{.generation = 1,
                       .order_type = OrderType::Ioc,
                       .side = Side::Sell,
                       .price = Price{2'905'500},
                       .quantity = Quantity{100},
                       .client_id = 7}));
    const Command market =
        decoded(client_message("order", R"(,"client_id":8,"side":"buy","order_type":"market","qty":10)")).command;
    EXPECT_EQ(market.order_type, OrderType::Market);
    EXPECT_EQ(market.price, Price{});
    EXPECT_EQ(decoded(client_message("cancel", R"(,"order_id":3)")).command,
              (Command{.generation = 1, .type = CommandType::Cancel, .order_id = 3}));

    const Command mm =
        decoded(client_message("attach", R"(,"strategy":"market_maker","params":{"quote_size":50})")).command;
    EXPECT_EQ(mm.type, CommandType::Attach);
    EXPECT_EQ(mm.strategy.kind, SessionStrategyKind::MarketMaker);
    EXPECT_EQ(mm.strategy.market_maker, (MarketMakerParams{.quote_size = 50}));  // the rest keep their defaults
    const Command taker =
        decoded(client_message("attach", R"(,"strategy":"imbalance","params":{"threshold":1.5,"clip":20})")).command;
    EXPECT_EQ(taker.strategy.imbalance, (ImbalanceTakerParams{.threshold = 1.5, .clip = 20}));
    EXPECT_EQ(decoded(client_message("attach", R"(,"strategy":"imbalance")")).command.strategy.imbalance,
              ImbalanceTakerParams{});
    EXPECT_EQ(decoded(client_message("attach", R"(,"strategy":"market_maker")")).command.strategy.market_maker,
              MarketMakerParams{});
    EXPECT_EQ(decoded(client_message("detach")).command.type, CommandType::Detach);

    const ClientMessage resync = decoded(client_message("resync", R"(,"from_report_seq":42)"));
    EXPECT_EQ(resync.type, ClientType::Resync);
    EXPECT_EQ(resync.from_report_seq, 42u);

    // The largest values the schema takes.
    const Command largest =
        decoded(client_message("order",
                               R"(,"client_id":18446744073709551615,"side":"buy","order_type":"ioc",)"
                               R"("price":9223372036854775807,"qty":9223372036854775807)"))
            .command;
    EXPECT_EQ(largest.client_id, 18'446'744'073'709'551'615u);
    EXPECT_EQ(largest.price, Price{std::numeric_limits<std::int64_t>::max()});
    EXPECT_EQ(largest.quantity, Quantity{9'223'372'036'854'775'807u});

    // A key given twice keeps its last value, as JSON.parse does.
    const Command twice =
        decoded(
            client_message("order",
                           R"(,"client_id":1,"client_id":2,"side":"sell","side":"buy","order_type":"market","qty":10)"))
            .command;
    EXPECT_EQ(twice.client_id, 2u);
    EXPECT_EQ(twice.side, Side::Buy);

    // Any printable ASCII but a space, 1 to 8 characters.
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"BRK.B")")).control.symbol),
              "BRK.B");
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"~!@#$%^&")")).control.symbol),
              "~!@#$%^&");
}

TEST(ProtocolTest, RefusesAnythingOutsideItsSchema)
{
    ClientMessage ignored;
    std::string error;
    EXPECT_FALSE(decode("{", ignored, error));
    EXPECT_EQ(error, "not a JSON object with a type");
    EXPECT_FALSE(decode(R"({"type":"order","qty":1})", ignored, error));
    EXPECT_EQ(error, "no session_id and generation");
    EXPECT_FALSE(decode(client_message("frobnicate"), ignored, error));
    EXPECT_EQ(error, "unknown type frobnicate");
    EXPECT_FALSE(decode(client_message(std::string(1'000, 'x')), ignored, error));
    EXPECT_EQ(error, "unknown type " + std::string(32, 'x'));  // at most 32 characters of the client's type

    EXPECT_TRUE(refused("[1]"));
    EXPECT_TRUE(refused(R"({"type":"auth","token":"t","extra":1})"));      // a key the message does not have
    EXPECT_TRUE(refused(R"({"type":"auth","token":5})"));                  // a value of the wrong type
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"rewind")")));    // an unknown name
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"play","n":1)")));  // a key its action does not take
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"step","n":0)")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"speed","speed":1001)")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"seek","ts":-1)")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"")")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"TOOLONGXX")")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"A B")")));        // a space
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"A\u0001")")));  // a control character
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"A\u007f")")));  // DEL
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"A\u0000")")));  // NUL
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":"\u00e9")")));  // not ASCII
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"symbol","symbol":5)")));

    const std::string buy = R"(,"client_id":1,"side":"buy","order_type":"post_only")";
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"qty":10)")));  // a limit order needs its price
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":0,"qty":10)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":0)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":-5)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":2.5)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":18446744073709551616)")));
    EXPECT_TRUE(
        refused(client_message("order", R"(,"client_id":1,"side":"buy","order_type":"market","price":1,"qty":1)")));
    EXPECT_TRUE(
        refused(client_message("order", R"(,"client_id":1,"side":"sel","order_type":"ioc","price":1,"qty":1)")));
    EXPECT_TRUE(refused(client_message("cancel", R"(,"order_id":"3")")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"none")")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"imbalance","params":{"threshold":0.5})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"imbalance","params":{"threshold":1e999})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"market_maker","params":{"clip":5})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"market_maker","params":[])")));
    EXPECT_TRUE(refused(client_message("resync")));

    // Hostile values.
    EXPECT_TRUE(refused(R"({"type":5})"));
    EXPECT_TRUE(refused(R"({"type":["auth"]})"));
    EXPECT_TRUE(refused(R"({"type":null})"));
    EXPECT_TRUE(refused("{\"type\":\"auth\",\"token\":\"\xff\"}"));  // invalid UTF-8
    EXPECT_TRUE(refused(R"({"type":"auth","token":"t","token":5})"));  // the last one counts
    EXPECT_TRUE(refused(client_message("order", R"(,"client_id":1,"side":0,"order_type":"ioc","price":1,"qty":1)")));
    EXPECT_TRUE(refused(client_message("order", R"(,"client_id":1,"side":"buy","order_type":2,"price":1,"qty":1)")));
    EXPECT_TRUE(
        refused(client_message("order", R"(,"client_id":-1,"side":"buy","order_type":"ioc","price":1,"qty":1)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":9223372036854775808)")));  // 2^63
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":9223372036854775808,"qty":10)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":-1,"qty":10)")));
    EXPECT_TRUE(refused(client_message("order", buy + R"(,"price":100,"qty":10,"qty":0)")));  // the last one counts
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"seek","ts":9223372036854775808)")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"step","n":9223372036854775808)")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"speed","speed":-1)")));
    EXPECT_TRUE(refused(client_message("resync", R"(,"from_report_seq":-1)")));
    EXPECT_TRUE(refused(client_message("cancel", R"(,"order_id":18446744073709551616)")));
    EXPECT_TRUE(refused(client_message("order", R"(,"client_id":1,"side":"buy","order_type":"ioc","price":1,"qty":1)") +
                        " x"));  // text after the message
    EXPECT_TRUE(refused(R"({"type":"auth","token":"t"} {"type":"auth","token":"t"})"));

    // Each strategy param: its cap is taken, anything over it is not.
    struct Cap
    {
        const char* strategy;
        const char* param;
        std::uint64_t cap;
    };
    const Cap caps[] = {{"market_maker", "quote_size", 1'000'000'000},
                        {"market_maker", "max_position", 1'000'000'000},
                        {"market_maker", "requote_cooldown_ns", 1'000'000'000'000'000},
                        {"market_maker", "improve_ticks", 1'000'000'000},
                        {"imbalance", "clip", 1'000'000'000},
                        {"imbalance", "max_position", 1'000'000'000},
                        {"imbalance", "cooldown_ns", 1'000'000'000'000'000}};
    for (const Cap& c : caps) {
        const auto attach = [&](std::uint64_t value) {
            return client_message("attach",
                                  std::string{R"(,"strategy":")"} + c.strategy + R"(","params":{")" + c.param +
                                      R"(":)" + std::to_string(value) + "}");
        };
        EXPECT_FALSE(refused(attach(c.cap))) << c.param;
        EXPECT_TRUE(refused(attach(c.cap + 1))) << c.param;
        EXPECT_TRUE(refused(attach(9'223'372'036'854'775'807u))) << c.param;  // int64 max
        EXPECT_TRUE(refused(attach(9'223'372'036'854'775'808u))) << c.param;  // 2^63
        EXPECT_TRUE(refused(attach(18'446'744'073'709'551'615u))) << c.param;
    }
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"imbalance","params":{"threshold":-1})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"imbalance","params":{"threshold":"2"})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"market_maker","params":{"quote_size":0})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"imbalance","params":{"clip":0})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"market_maker","params":{"max_position":-1})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"market_maker","params":null)")));

    // At most 64 KiB.
    const std::string prefix = R"({"type":"auth","token":")";
    const std::string at_limit = prefix + std::string(max_message_bytes - prefix.size() - 2, 'x') + R"("})";
    ASSERT_EQ(at_limit.size(), max_message_bytes);
    EXPECT_EQ(decoded(at_limit).type, ClientType::Auth);
    EXPECT_FALSE(decode(at_limit + " ", ignored, error));
    EXPECT_EQ(error, "over 64 KiB");
}

// A client can nest 32,000 arrays within 64 KiB. Decoding must not recurse through that: peers decode before auth, on
// threads with the default stack, which is what this test runs on.
TEST(ProtocolTest, DecodingDoesNotRecurseThroughAClientsNesting)
{
    const std::string deep = std::string(32'000, '[') + std::string(32'000, ']');
    const std::vector<std::string> texts{
        client_message("attach", R"(,"strategy":"market_maker","params":{"x":)" + deep + "}"),
        client_message("attach", R"(,"strategy":"imbalance","params":{"x":)" + deep + "}"),
        client_message("attach", R"(,"strategy":"imbalance","params":)" + deep),
        client_message("order", R"(,"client_id":1,"side":"buy","order_type":"market","qty":1,"x":)" + deep),
        R"({"type":"auth","token":"t","x":)" + deep + "}"};
    std::vector<bool> accepted;
    std::thread([&] {
        for (const std::string& text : texts) {
            ClientMessage message;
            std::string error;
            accepted.push_back(decode(text, message, error));
        }
    }).join();
    ASSERT_EQ(accepted.size(), texts.size());
    for (std::size_t i = 0; i < texts.size(); ++i) {
        EXPECT_LE(texts[i].size(), max_message_bytes) << i;
        EXPECT_FALSE(accepted[i]) << i;
    }
}

TEST(ProtocolTest, EncodesSnapshotsRecordsAndReplies)
{
    Snapshot s;
    s.generation = 3;
    s.symbol = ticker("AAPL");
    s.snapshot_seq = 9;
    s.exchange_ts = 1'577'716'200'000'000'123;
    s.event_seq = 11;
    s.last_report_seq = 8;
    s.last_command_seq = 2;
    s.state = SessionState::Playing;
    s.speed = 10;
    s.progress = 0.5;
    s.source_error = SourceError::None;
    s.bids[0] = LevelInfo{.price = Price{2'905'500}, .quantity = 300, .order_count = 2};
    s.bid_levels = 1;
    s.asks[0] = LevelInfo{.price = Price{2'905'600}, .quantity = 200, .order_count = 1};
    s.ask_levels = 1;
    s.trades[0] = Trade{.timestamp = 5, .price = Price{2'905'600}, .quantity = 100, .side = Side::Sell};
    s.trade_count = 1;
    s.trades_truncated = true;
    s.has_candle = true;
    s.candle =
        Candle{.start = kSecond, .open = Price{1}, .high = Price{4}, .low = Price{1}, .close = Price{2}, .volume = 7};
    s.closed_candles[0] =
        Candle{.start = 0, .open = Price{3}, .high = Price{5}, .low = Price{2}, .close = Price{4}, .volume = 9};
    s.closed_count = 1;
    s.candles_truncated = true;
    s.account = Account{
        .position = 5, .cash = -1000, .realised = 3, .unrealised = 4, .fees = 2, .pnl = -25, .max_drawdown = 40};
    s.metrics = SnapshotMetrics{
        .events_per_second = 1234, .dropped_snapshots = 3, .command_queue_depth = 6, .log_failed = false};
    s.my_orders[0] =
        MyOrder{.id = 4, .price = Price{2'905'500}, .remaining = 10, .ahead_quantity = 290, .ahead_orders = 2};
    s.my_order_count = 1;
    s.my_orders_truncated = true;
    const json snapshot = json::parse(encode_snapshot("s1", s, false, false));
    EXPECT_EQ(snapshot["type"], "snapshot");
    EXPECT_EQ(snapshot["session_id"], "s1");
    EXPECT_EQ(snapshot["generation"], 3);
    EXPECT_EQ(snapshot["symbol"], "AAPL");
    EXPECT_EQ(snapshot["snapshot_seq"], 9);
    EXPECT_EQ(snapshot["exchange_ts"], 1'577'716'200'000'000'123);
    EXPECT_EQ(snapshot["state"], "playing");
    EXPECT_EQ(snapshot["bids"], json::parse("[[2905500,300,2]]"));
    EXPECT_EQ(snapshot["asks"], json::parse("[[2905600,200,1]]"));
    EXPECT_EQ(snapshot["trades"], json::parse(R"([[5,2905600,100,"sell"]])"));
    EXPECT_EQ(snapshot["candle"], json::parse("[1000000000,1,4,1,2,7]"));
    EXPECT_EQ(snapshot["closed_candles"], json::parse("[[0,3,5,2,4,9]]"));
    EXPECT_EQ(snapshot["account"],
              json::parse(R"({"position":5,"cash":-1000,"realised":3,"unrealised":4,"fees":2,"pnl":-25,)"
                          R"("max_drawdown":40})"));
    EXPECT_EQ(snapshot["my_orders"],
              json::parse(R"([{"id":4,"side":"buy","price":2905500,"remaining":10,)"
                          R"("ahead_quantity":290,"ahead_orders":2}])"));
    EXPECT_EQ(snapshot["failure"], "none");
    EXPECT_EQ(snapshot["source_error"], "None");
    EXPECT_EQ(snapshot["metrics"],
              json::parse(R"({"events_per_second":1234,"dropped_snapshots":3,"command_queue_depth":6,)"
                          R"("log_failed":false})"));
    EXPECT_EQ(snapshot["event_seq"], 11);
    EXPECT_EQ(snapshot["last_report_seq"], 8);
    EXPECT_EQ(snapshot["last_command_seq"], 2);
    EXPECT_EQ(snapshot["speed"], 10);
    EXPECT_EQ(snapshot["progress"], 0.5);
    EXPECT_EQ(snapshot["trades_truncated"], true);
    EXPECT_EQ(snapshot["candles_truncated"], true);
    EXPECT_EQ(snapshot["my_orders_truncated"], true);
    // Exactly these keys: one a client reads is one the encoder must keep writing.
    const auto keys_of = [](const json& object) {
        std::set<std::string> keys;
        for (const auto& item : object.items()) {
            keys.insert(item.key());
        }
        return keys;
    };
    std::set<std::string> snapshot_keys{"type",      "session_id",      "generation",
                                        "symbol",    "snapshot_seq",    "exchange_ts",
                                        "event_seq", "last_report_seq", "last_command_seq",
                                        "state",     "speed",           "progress",
                                        "failure",   "source_error",    "bids",
                                        "asks",      "trades",          "trades_truncated",
                                        "candle",    "closed_candles",  "candles_truncated",
                                        "account",   "my_orders",       "my_orders_truncated",
                                        "metrics"};
    EXPECT_EQ(keys_of(snapshot), snapshot_keys);
    EXPECT_FALSE(snapshot.contains("full_resync"));
    Snapshot failed = s;
    failed.has_candle = false;
    failed.failure = ReplayFailure::OutOfOrderTimestamp;
    failed.source_error = SourceError::TruncatedMessage;
    failed.metrics.log_failed = true;
    const json state = json::parse(encode_snapshot("s1", failed, true, true));
    EXPECT_EQ(state["type"], "state");
    EXPECT_EQ(state["full_resync"], true);
    EXPECT_EQ(state["failure"], "out_of_order_timestamp");
    EXPECT_EQ(state["source_error"], to_string(SourceError::TruncatedMessage));
    EXPECT_EQ(state["candle"], nullptr);
    EXPECT_EQ(state["metrics"]["log_failed"], true);
    snapshot_keys.insert("full_resync");
    EXPECT_EQ(keys_of(state), snapshot_keys);

    const json step = json::parse(encode_record("s1",
                                                Record{.seq = 5,
                                                       .generation = 3,
                                                       .step = Lifecycle::RejectedAtApply,
                                                       .reason = ErrorCode::RiskPositionLimit,
                                                       .command = {.command_seq = 2, .client_id = 77}}));
    EXPECT_EQ(step,
              json::parse(R"({"type":"report","session_id":"s1","generation":3,"seq":5,"kind":"lifecycle",)"
                          R"("step":"rejected_at_apply","reason":"risk_position_limit","command":"order",)"
                          R"("command_seq":2,"client_id":77,"order_id":0,"exchange_ts":0,"market_event_seq":0})"));
    const json fill = json::parse(encode_record("s1",
                                                Record{.seq = 6,
                                                       .generation = 3,
                                                       .kind = RecordKind::Report,
                                                       .report = {.exchange_time = 9,
                                                                  .order_id = 4,
                                                                  .price = Price{2'905'500},
                                                                  .quantity = Quantity{10},
                                                                  .leaves = Quantity{5},
                                                                  .type = ReportType::Fill,
                                                                  .liquidity = Liquidity::Maker,
                                                                  .side = Side::Sell}}));
    EXPECT_EQ(fill,
              json::parse(R"({"type":"report","session_id":"s1","generation":3,"seq":6,"kind":"execution",)"
                          R"("order_id":4,"report_type":"fill","side":"sell","price":2905500,"quantity":10,)"
                          R"("leaves":5,"reject_reason":"none","liquidity":"maker","exchange_ts":9})"));
    const json rejected = json::parse(encode_record(
        "s1",
        Record{.seq = 10,
               .generation = 3,
               .kind = RecordKind::Report,
               .report = {.order_id = 5, .type = ReportType::Rejected, .reason = RejectReason::WouldCross}}));
    EXPECT_EQ(rejected["report_type"], "rejected");
    EXPECT_EQ(rejected["reject_reason"], "would_cross");
    EXPECT_EQ(rejected["side"], "buy");
    EXPECT_EQ(rejected["price"], 0);
    EXPECT_EQ(rejected["leaves"], 0);
    const json ack = json::parse(encode_record("s1",
                                               Record{.seq = 7,
                                                      .generation = 3,
                                                      .kind = RecordKind::Ack,
                                                      .control = {.control_seq = 9, .type = ControlType::Seek},
                                                      .state = SessionState::Seeking}));
    EXPECT_EQ(ack,
              json::parse(R"({"type":"ack","session_id":"s1","generation":3,"seq":7,"control":"seek",)"
                          R"("control_seq":9,"state":"seeking"})"));
    const json start =
        json::parse(encode_record("s1",
                                  Record{.seq = 8,
                                         .generation = 4,
                                         .kind = RecordKind::GenerationStart,
                                         .command = {.strategy = {.kind = SessionStrategyKind::MarketMaker}},
                                         .control = {.symbol = ticker("MSFT")}}));
    EXPECT_EQ(start["kind"], "generation_start");
    EXPECT_EQ(start["symbol"], "MSFT");
    EXPECT_EQ(start["strategy"], "market_maker");
    const json end = json::parse(encode_record(
        "s1",
        Record{
            .seq = 9, .generation = 4, .kind = RecordKind::GenerationEnd, .exchange_ts = 99, .market_event_seq = 3}));
    EXPECT_EQ(end["kind"], "generation_end");
    EXPECT_EQ(end["market_event_seq"], 3);

    EXPECT_EQ(json::parse(encode_hello("s1", 3, true, 4)),
              json::parse(R"({"type":"hello","session_id":"s1","generation":3,"protocol":1,"role":"controller",)"
                          R"("price_scale":4})"));
    EXPECT_EQ(json::parse(encode_hello("s1", 3, false, 4))["role"], "viewer");
    EXPECT_EQ(json::parse(encode_command_ack("s1", 3, 12, true))["persisted"], "fsynced");
    EXPECT_EQ(json::parse(encode_command_ack("s1", 3, 12, false)),
              json::parse(R"({"type":"ack","session_id":"s1","generation":3,"command_seq":12,"persisted":"flushed"})"));
    EXPECT_EQ(json::parse(encode_gap("s1", 3, 1, 70)),
              json::parse(R"({"type":"gap","session_id":"s1","generation":3,"from":1,"to":70})"));
    EXPECT_EQ(json::parse(encode_progress("s1", 3, 500))["seek"], 500);
    EXPECT_EQ(json::parse(encode_error("s1", 3, "queue_full", "order refused", 12)),
              json::parse(R"({"type":"error","session_id":"s1","generation":3,"code":"queue_full",)"
                          R"("message":"order refused","command_seq":12})"));
    EXPECT_FALSE(json::parse(encode_error("s1", 3, "bad_request", "not JSON")).contains("command_seq"));

    const std::vector<std::string> symbols{"AAPL", "MSFT"};
    EXPECT_EQ(json::parse(encode_symbols(symbols)), json::parse(R"({"symbols":["AAPL","MSFT"]})"));
    const std::vector<Candle> bars{s.candle};
    const std::vector<Trade> trades{s.trades[0]};
    EXPECT_EQ(json::parse(encode_history("s1", 3, bars, trades)),
              json::parse(R"({"session_id":"s1","generation":3,"candles":[[1000000000,1,4,1,2,7]],)"
                          R"("trades":[[5,2905600,100,"sell"]]})"));
}

}  // namespace
}  // namespace simulator::server::test
