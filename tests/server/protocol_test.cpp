#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/backtest.hpp"
#include "chronos/server/itch_input.hpp"
#include "chronos/server/lab.hpp"
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
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"AAPL")")).control.spec.ticker),
              "AAPL");
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
    const Command orb =
        decoded(client_message("attach", R"(,"strategy":"orb","params":{"range_minutes":30,"clip":50})")).command;
    EXPECT_EQ(orb.strategy.kind, SessionStrategyKind::Orb);
    EXPECT_EQ(orb.strategy.orb, (OrbParams{.range_minutes = 30, .clip = 50}));
    EXPECT_EQ(decoded(client_message("attach", R"(,"strategy":"orb")")).command.strategy.orb, OrbParams{});
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
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"BRK.B")")).control.spec.ticker),
              "BRK.B");
    EXPECT_EQ(name(decoded(client_message("control", R"(,"action":"symbol","symbol":"~!@#$%^&")")).control.spec.ticker),
              "~!@#$%^&");
}

// A load names 1 to 32 days, strictly increasing; without the key it names none, and the Peer fills them in.
TEST(ProtocolTest, DecodesTheDaysOfASymbolLoad)
{
    const auto load = [](const std::string& days) {
        ClientMessage message;
        std::string error;
        const bool valid =
            decode(client_message("control", R"(,"action":"symbol","symbol":"SPY")" + days), message, error);
        return valid ? std::optional<DayList>{message.control.spec.days} : std::nullopt;
    };
    using namespace std::chrono_literals;
    EXPECT_EQ(load(""), DayList{});
    EXPECT_EQ(load(R"(,"days":["2019-12-27","2019-12-30"])"),
              (DayList{.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2}));
    std::string all = R"(,"days":[)";
    for (int day = 1; day <= 32; ++day) {
        all +=
            (day > 1 ? ",\"" : "\"") + iso_date(std::chrono::sys_days{2019y / 1 / 1} + std::chrono::days{day}) + "\"";
    }
    EXPECT_EQ(load(all + "]")->count, 32u);
    EXPECT_FALSE(load(all + R"(,"2019-03-01"])"));  // 33 days
    EXPECT_FALSE(load(R"(,"days":[])"));
    EXPECT_FALSE(load(R"(,"days":["2019-12-30","2019-12-27"])"));  // not increasing
    EXPECT_FALSE(load(R"(,"days":["2019-12-30","2019-12-30"])"));
    EXPECT_FALSE(load(R"(,"days":["2019-02-30"])"));
    EXPECT_FALSE(load(R"(,"days":[20191230])"));
    EXPECT_FALSE(load(R"(,"days":"2019-12-30")"));
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
    EXPECT_EQ(error, "unknown type " + std::string(32, 'x'));  // at most 32 bytes of the client's type

    // A refused order keeps the client_id it carried, for the error that answers it; other messages carry none.
    EXPECT_FALSE(decode(client_message("order", R"(,"client_id":0,"side":"sel","order_type":"ioc","price":1,"qty":1)"),
                        ignored, error));
    EXPECT_EQ(ignored.client_id, std::optional<std::uint64_t>{0});
    EXPECT_FALSE(decode(client_message("order", R"(,"client_id":-1,"qty":1)"), ignored, error));
    EXPECT_FALSE(ignored.client_id.has_value());
    EXPECT_FALSE(decoded(client_message("cancel", R"(,"order_id":3)")).client_id.has_value());

    EXPECT_TRUE(refused("[1]"));
    EXPECT_TRUE(refused(R"({"type":"auth","token":"t","extra":1})"));      // a key the message does not have
    EXPECT_TRUE(refused(R"({"type":"auth","token":5})"));                  // a value of the wrong type
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"rewind")")));    // an unknown name
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"play","n":1)")));  // a key its action does not take
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"play","":1)")));   // a key that is itself empty
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
                        {"imbalance", "cooldown_ns", 1'000'000'000'000'000},
                        {"orb", "range_minutes", 390},
                        {"orb", "clip", 1'000'000'000}};
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
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"orb","params":{"range_minutes":0})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"orb","params":{"clip":0})")));
    EXPECT_TRUE(refused(client_message("attach", R"(,"strategy":"orb","params":{"threshold":2})")));

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
    s.spec.ticker = ticker("AAPL");
    s.snapshot_seq = 9;
    s.exchange_ts = 1'577'716'200'000'000'123;
    s.event_seq = 11;
    s.last_report_seq = 8;
    s.last_command_seq = 2;
    s.state = SessionState::Playing;
    s.speed = 10;
    s.progress = 0.5;
    s.source_error = SourceError::None;
    s.trading_state = TradingState::QuotationOnly;
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
    EXPECT_EQ(snapshot["trading_state"], "quotation_only");
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
    std::set<std::string> snapshot_keys{"type",
                                        "session_id",
                                        "generation",
                                        "symbol",
                                        "snapshot_seq",
                                        "exchange_ts",
                                        "event_seq",
                                        "last_report_seq",
                                        "last_command_seq",
                                        "state",
                                        "speed",
                                        "progress",
                                        "failure",
                                        "source_error",
                                        "trading_state",
                                        "bids",
                                        "asks",
                                        "trades",
                                        "trades_truncated",
                                        "candle",
                                        "closed_candles",
                                        "candles_truncated",
                                        "account",
                                        "my_orders",
                                        "my_orders_truncated",
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
    failed.failure = ReplayFailure::CapacityExhausted;
    EXPECT_EQ(json::parse(encode_snapshot("s1", failed, false, false))["failure"], "capacity_exhausted");
    snapshot_keys.insert({"full_resync", "spec", "run_id"});
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
                                                                  .side = Side::Sell,
                                                                  .cause = FillCause::RuleCReplace,
                                                                  .entry_ahead_quantity = 290}}));
    EXPECT_EQ(fill,
              json::parse(R"({"type":"report","session_id":"s1","generation":3,"seq":6,"kind":"execution",)"
                          R"("order_id":4,"report_type":"fill","side":"sell","price":2905500,"quantity":10,)"
                          R"("leaves":5,"reject_reason":"none","liquidity":"maker","exchange_ts":9,)"
                          R"("rule":"c_replace","ahead_at_entry":290})"));
    const json taker = json::parse(encode_record(
        "s1", Record{.kind = RecordKind::Report, .report = {.type = ReportType::Fill, .liquidity = Liquidity::Taker}}));
    EXPECT_FALSE(taker.contains("rule"));
    EXPECT_FALSE(taker.contains("ahead_at_entry"));
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
    EXPECT_FALSE(rejected.contains("rule"));
    const json ack = json::parse(encode_record("s1",
                                               Record{.seq = 7,
                                                      .generation = 3,
                                                      .kind = RecordKind::Ack,
                                                      .control = {.control_seq = 9, .type = ControlType::Seek},
                                                      .state = SessionState::Seeking}));
    EXPECT_EQ(ack,
              json::parse(R"({"type":"ack","session_id":"s1","generation":3,"seq":7,"control":"seek",)"
                          R"("control_seq":9,"state":"seeking"})"));
    const json start = json::parse(encode_record(
        "s1",
        Record{
            .seq = 8,
            .generation = 4,
            .kind = RecordKind::GenerationStart,
            .control = {.spec = {.ticker = ticker("MSFT"), .strategy = {.kind = SessionStrategyKind::MarketMaker}}}}));
    EXPECT_EQ(start["kind"], "generation_start");
    EXPECT_EQ(start["symbol"], "MSFT");
    EXPECT_EQ(start["strategy"], "market_maker");
    const json end = json::parse(encode_record(
        "s1",
        Record{
            .seq = 9, .generation = 4, .kind = RecordKind::GenerationEnd, .exchange_ts = 99, .market_event_seq = 3}));
    EXPECT_EQ(end["kind"], "generation_end");
    EXPECT_EQ(end["market_event_seq"], 3);

    SessionConfig session = config();
    session.defaults.fees = {.maker_fee_per_share = -1, .taker_fee_per_share = 3};
    EXPECT_EQ(json::parse(encode_hello("s1", 3, true, session)),
              json::parse(R"({"type":"hello","session_id":"s1","generation":3,"protocol":2,"role":"controller",)"
                          R"("price_scale":4,"quantity_scale":0,"fees":{"maker":-1,"taker":3},)"
                          R"("limits":{"max_position":1000,"max_order_quantity":500,"max_open_orders":4}})"));
    EXPECT_EQ(json::parse(encode_hello("s1", 3, false, session))["role"], "viewer");
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
    EXPECT_FALSE(json::parse(encode_error("s1", 3, "bad_request", "not JSON")).contains("client_id"));
    EXPECT_EQ(json::parse(encode_error("s1", 3, "rate_limited", "too many", 0, 0))["client_id"], 0);  // 0 is an id

    const std::vector<std::string> symbols{"AAPL", "MSFT"};
    EXPECT_EQ(json::parse(encode_symbols(symbols)), json::parse(R"({"symbols":["AAPL","MSFT"]})"));
    const std::vector<Candle> bars{s.candle};
    const std::vector<Trade> trades{s.trades[0]};
    EXPECT_EQ(json::parse(encode_history("s1", 3, bars, trades)),
              json::parse(R"({"session_id":"s1","generation":3,"candles":[[1000000000,1,4,1,2,7]],)"
                          R"("trades":[[5,2905600,100,"sell"]]})"));
}

// Protocol 2's additions: generation_start's days, window and run id, the state's spec, and a source error's day.
TEST(ProtocolTest, EncodesTheGenerationsSpecAndTheDayASourceErrorIsOn)
{
    using namespace std::chrono_literals;
    const RunSpec spec{.ticker = ticker("SPY"),
                       .days = {.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2},
                       .latency = {.order_latency = 50'000, .report_latency = 40'000},
                       .fees = {.maker_fee_per_share = -20, .taker_fee_per_share = 30},
                       .limits = {.max_position = 500, .max_order_quantity = Quantity{100}, .max_open_orders = 2},
                       .strategy = {.kind = SessionStrategyKind::MarketMaker,
                                    .market_maker = {.quote_size = 100, .max_position = 500}}};
    const json start = json::parse(encode_record(
        "s1", Record{.seq = 8, .generation = 4, .kind = RecordKind::GenerationStart, .control = {.spec = spec}}));
    EXPECT_EQ(start["days"], json::parse(R"(["2019-12-27","2019-12-30"])"));
    EXPECT_EQ(start["window"], "full_day");
    EXPECT_EQ(start["run_id"], "");

    Snapshot s;
    s.spec = spec;
    s.exchange_ts = session_midnight_ns(2019y / 12 / 30, -5h) + kSecond;
    s.source_error = SourceError::TruncatedMessage;
    const json state = json::parse(encode_snapshot("s1", s, true, false));
    EXPECT_EQ(
        state["spec"],
        json::parse(
            R"({"symbol":"SPY","days":["2019-12-27","2019-12-30"],"strategy":"market_maker",)"
            R"("params":{"quote_size":100,"max_position":500,"requote_cooldown_ns":100000000,"improve_ticks":0},)"
            R"("latency":{"order_ns":50000,"report_ns":40000},"fees":{"maker":-20,"taker":30},)"
            R"("limits":{"max_position":500,"max_order_quantity":100,"max_open_orders":2},"window":"full_day"})"));
    EXPECT_EQ(state["run_id"], "");
    EXPECT_EQ(state["source_error"], "TruncatedMessage on 2019-12-30");
    s.exchange_ts -= kSecond;  // Monday's midnight itself, where its first message fails
    EXPECT_EQ(json::parse(encode_snapshot("s1", s, false, false))["source_error"], "TruncatedMessage on 2019-12-30");
    s.exchange_ts -= kSecond;  // still Friday's
    EXPECT_EQ(json::parse(encode_snapshot("s1", s, false, false))["source_error"], "TruncatedMessage on 2019-12-27");
    s.spec.days.count = 1;  // one day: as UI-1 named it
    EXPECT_EQ(json::parse(encode_snapshot("s1", s, false, false))["source_error"], "TruncatedMessage");
    EXPECT_EQ(json::parse(encode_snapshot("s1", Snapshot{}, true, false))["spec"]["params"], json::object());
}

// A Lab run's spec, decoded; a test failure, naming the field, if it is refused.
RunSpec run_spec(const std::string& text)
{
    RunSpec spec;
    std::string field;
    EXPECT_TRUE(decode_run_spec(text, spec, field)) << text << ": " << field;
    return spec;
}

// The field decode_run_spec() names for `text`, which it must refuse.
std::string bad_field(const std::string& text)
{
    RunSpec spec;
    std::string field = "unset";
    EXPECT_FALSE(decode_run_spec(text, spec, field)) << text;
    return field;
}

// An ORB whose clip is above the position or order limit could never enter: refused, naming its clip, as the CLI
// refuses it.
TEST(ProtocolTest, RefusesAnOrbRunWhoseClipIsAboveThePositionLimit)
{
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"orb","params":{"clip":600}})"),
              "params.clip");
    const RunSpec spec = run_spec(
        R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"orb","params":{"clip":600},"limits":{"max_position":600,)"
        R"("max_order_quantity":600}})");
    EXPECT_EQ(spec.strategy.orb.clip, 600u);
    const std::string order_limit =
        R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"orb","limits":{"max_order_quantity":50}})";
    EXPECT_EQ(bad_field(order_limit), "params.clip");  // the default clip of 100, above the order limit
}

// A run's symbol, days and strategy are required; every other key takes `simulator backtest`'s default, and an omitted
// limit follows the params as the CLI's does.
TEST(ProtocolTest, DecodesARunSpecWithTheClisDefaults)
{
    using namespace std::chrono_literals;
    RunSpec expected = default_spec(SessionStrategyKind::MarketMaker);
    expected.ticker = ticker("SPY");
    expected.days = {.day = {2019y / 12 / 27, 2019y / 12 / 30}, .count = 2};
    const RunSpec minimal =
        run_spec(R"({"symbol":"SPY","days":["2019-12-27","2019-12-30"],"strategy":"market_maker"})");
    EXPECT_EQ(json::parse(encode_snapshot("s", Snapshot{.spec = minimal}, true, false))["spec"],
              json::parse(encode_snapshot("s", Snapshot{.spec = expected}, true, false))["spec"]);
    EXPECT_EQ(minimal.window, TradingWindow::Regular);
    EXPECT_EQ(minimal.latency.order_latency, 50'000);
    EXPECT_EQ(minimal.limits.max_open_orders, 2u);

    RunSpec expected_imbalance = default_spec(SessionStrategyKind::ImbalanceTaker);
    expected_imbalance.ticker = ticker("SPY");
    expected_imbalance.days = {.day = {2019y / 12 / 30}, .count = 1};
    const RunSpec imbalance = run_spec(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"imbalance"})");
    EXPECT_EQ(json::parse(encode_snapshot("s", Snapshot{.spec = imbalance}, true, false))["spec"],
              json::parse(encode_snapshot("s", Snapshot{.spec = expected_imbalance}, true, false))["spec"]);
    EXPECT_EQ(imbalance.limits.max_open_orders, 1u);

    const RunSpec sized = run_spec(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"market_maker",)"
                                   R"("params":{"quote_size":200,"max_position":800}})");
    EXPECT_EQ(sized.limits.max_order_quantity, Quantity{200});  // as --quote-size 200 --max-position 800 set them
    EXPECT_EQ(sized.limits.max_position, 800);
    EXPECT_EQ(sized.strategy.market_maker.requote_cooldown_ns, 100'000'000);

    const RunSpec full = run_spec(
        R"({"symbol":"AAPL","days":["2019-12-30"],"strategy":"imbalance",)"
        R"("params":{"threshold":2.5,"clip":50,"max_position":300,"cooldown_ns":0},)"
        R"("latency":{"order_ns":1000000000000,"report_ns":0},"fees":{"maker":-1000000,"taker":1000000},)"
        R"("limits":{"max_position":1000000000,"max_order_quantity":7,"max_open_orders":3},"window":"full_day"})");
    EXPECT_EQ(name(full.ticker), "AAPL");
    EXPECT_EQ(full.strategy.kind, SessionStrategyKind::ImbalanceTaker);
    EXPECT_EQ(full.strategy.imbalance,
              (ImbalanceTakerParams{.threshold = 2.5, .clip = 50, .max_position = 300, .cooldown_ns = 0}));
    EXPECT_EQ(full.latency.order_latency, 1'000'000'000'000);
    EXPECT_EQ(full.latency.report_latency, 0);
    EXPECT_EQ(full.fees.maker_fee_per_share, -1'000'000);
    EXPECT_EQ(full.fees.taker_fee_per_share, 1'000'000);
    EXPECT_EQ(full.limits.max_position, 1'000'000'000);
    EXPECT_EQ(full.limits.max_order_quantity, Quantity{7});
    EXPECT_EQ(full.limits.max_open_orders, 3u);
    EXPECT_EQ(full.window, TradingWindow::FullDay);
}

// Keys are exact and every value is in its range; a refusal names its field.
TEST(ProtocolTest, RefusesABadRunSpecNamingItsField)
{
    using namespace std::chrono_literals;
    const std::string head = R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"market_maker")";
    EXPECT_EQ(bad_field("["), "");
    EXPECT_EQ(bad_field("[1]"), "");
    EXPECT_EQ(bad_field(head + R"(,"frob":1})"), "frob");
    EXPECT_EQ(bad_field(head + R"(,"":1})"), "");  // a key that is itself empty is an unknown key, not no key
    EXPECT_EQ(bad_field(R"({"days":["2019-12-30"],"strategy":"market_maker"})"), "symbol");
    EXPECT_EQ(bad_field(R"({"symbol":"TOOLONGXX","days":["2019-12-30"],"strategy":"market_maker"})"), "symbol");
    EXPECT_EQ(bad_field(R"({"symbol":"A B","days":["2019-12-30"],"strategy":"market_maker"})"), "symbol");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","strategy":"market_maker"})"), "days");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":[],"strategy":"market_maker"})"), "days");
    std::string many = R"({"symbol":"SPY","strategy":"market_maker","days":[)";
    for (int day = 0; day < 33; ++day) {
        const std::chrono::year_month_day date{std::chrono::sys_days{2019y / 1 / 1} + std::chrono::days{day}};
        many += (day == 0 ? "\"" : ",\"") + iso_date(date) + "\"";
    }
    EXPECT_EQ(bad_field(many + "]}"), "days");  // 33 days
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30","2019-12-27"],"strategy":"market_maker"})"), "days[1]");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30","2019-02-30"],"strategy":"market_maker"})"), "days[1]");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30"]})"), "strategy");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"none"})"), "strategy");
    EXPECT_EQ(bad_field(head + R"(,"params":[1]})"), "params");
    EXPECT_EQ(bad_field(head + R"(,"params":{"":1}})"), "params.");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"imbalance","params":{"":1}})"), "params.");
    EXPECT_EQ(bad_field(head + R"(,"params":{"quote_size":0}})"), "params.quote_size");
    EXPECT_EQ(bad_field(head + R"(,"params":{"quote_size":1000000001}})"), "params.quote_size");
    EXPECT_EQ(bad_field(head + R"(,"params":{"clip":5}})"), "params.clip");
    EXPECT_EQ(bad_field(head + R"(,"params":{"requote_cooldown_ns":-1}})"), "params.requote_cooldown_ns");
    EXPECT_EQ(bad_field(R"({"symbol":"SPY","days":["2019-12-30"],"strategy":"imbalance","params":{"threshold":0.5}})"),
              "params.threshold");
    EXPECT_EQ(bad_field(head + R"(,"latency":5})"), "latency");
    EXPECT_EQ(bad_field(head + R"(,"latency":{"order_ns":1000000000001}})"), "latency.order_ns");
    EXPECT_EQ(bad_field(head + R"(,"latency":{"report_ns":2.5}})"), "latency.report_ns");
    EXPECT_EQ(bad_field(head + R"(,"latency":{"order_us":5}})"), "latency.order_us");
    EXPECT_EQ(bad_field(head + R"(,"latency":{"":5}})"), "latency.");
    EXPECT_EQ(bad_field(head + R"(,"fees":{"maker":-1000001}})"), "fees.maker");
    EXPECT_EQ(bad_field(head + R"(,"fees":{"maker":18446744073709551615}})"), "fees.maker");
    EXPECT_EQ(bad_field(head + R"(,"fees":{"taker":-1}})"), "fees.taker");
    EXPECT_EQ(bad_field(head + R"(,"limits":{"max_position":1000000001}})"), "limits.max_position");
    EXPECT_EQ(bad_field(head + R"(,"limits":{"max_order_quantity":0}})"), "limits.max_order_quantity");
    EXPECT_EQ(bad_field(head + R"(,"limits":{"max_open_orders":0}})"), "limits.max_open_orders");
    EXPECT_EQ(bad_field(head + R"(,"window":"nightly"})"), "window");
}

// GET /api/strategies: the defaults a run's omitted keys take, and every cap the decoder applies.
TEST(ProtocolTest, EncodesTheStrategiesWithTheirDefaultsAndCaps)
{
    const json j = json::parse(encode_strategies());
    ASSERT_EQ(j["strategies"].size(), 3u);
    const json& mm = j["strategies"][0];
    EXPECT_EQ(mm["kind"], "market_maker");
    EXPECT_EQ(mm["params"][0], json::parse(R"({"name":"quote_size","default":100,"min":1,"max":1000000000})"));
    EXPECT_EQ(mm["params"][2]["default"], 100'000'000);
    EXPECT_EQ(mm["limits"], json::parse(R"({"max_position":500,"max_order_quantity":100,"max_open_orders":2})"));
    const json& imbalance = j["strategies"][1];
    EXPECT_EQ(imbalance["kind"], "imbalance");
    EXPECT_EQ(imbalance["params"][0], json::parse(R"({"name":"threshold","default":3.0,"min":1,"max":null})"));
    EXPECT_EQ(imbalance["limits"]["max_open_orders"], 1);
    const json& orb = j["strategies"][2];
    EXPECT_EQ(orb["kind"], "orb");
    EXPECT_EQ(orb["params"][0], json::parse(R"({"name":"range_minutes","default":15,"min":1,"max":390})"));
    EXPECT_EQ(orb["params"][1], json::parse(R"({"name":"clip","default":100,"min":1,"max":1000000000})"));
    EXPECT_EQ(orb["limits"], json::parse(R"({"max_position":500,"max_order_quantity":100,"max_open_orders":1})"));
    EXPECT_EQ(j["latency"], json::parse(R"({"order_ns":50000,"report_ns":50000,"max":1000000000000})"));
    EXPECT_EQ(j["fees"], json::parse(R"({"maker":0,"taker":0,"min":-1000000,"max":1000000,"taker_min":0})"));
    EXPECT_EQ(j["limit_max"], 1'000'000'000);
    EXPECT_EQ(j["windows"],
              json::parse(R"j([{"id":"regular","label":"Regular (feed session)"},)j"
                          R"j({"id":"full_day","label":"Full day"}])j"));
    EXPECT_EQ(j["max_days"], 32);
    EXPECT_EQ(j["max_queued"], 8);
}

// A finished run reads back from its file as it was written, whatever state it ended in; reproduced is true once a
// same-code, same-input verification matched, and false once one did not. The listing leaves the results out.
TEST(ProtocolTest, ARunDocumentReadsBackAsItWasWritten)
{
    using namespace std::chrono_literals;
    // Every field the document holds is off its default, so one it drops or misreads shows in the comparisons.
    RunResult results{.events = 9,
                      .sim_events = 4,
                      .pnl = -200,
                      .realised = -150,
                      .unrealised = -30,
                      .fees = 20,
                      .position = 100,
                      .max_abs_position = 120,
                      .max_drawdown = 260,
                      .maker_fills = 1,
                      .taker_fills = 1,
                      .filled_shares = 150,
                      .fill_rate = 0.5,
                      .digests = {.book = 0xcbf29ce484222325, .execution = 1, .portfolio = 2},
                      .equity = {{.second = 1'577'716'201, .pnl = 0, .position = 0},
                                 {.second = 1'577'716'202, .pnl = -200, .position = 100}},
                      .by_hour = {{9, -200}},
                      .by_day = {{.date = 2019y / 12 / 30, .pnl = -200, .fills = 1}},
                      .fills = {{.exchange_ts = 1'577'716'201'500'000'000,
                                 .order_id = 1,
                                 .price = Price{100'000},
                                 .quantity = Quantity{100},
                                 .liquidity = Liquidity::Maker,
                                 .rule = FillCause::RuleAExecute,
                                 .ahead_at_entry = 300,
                                 .position = 100},
                                {.exchange_ts = 1'577'716'202'000'000'000,
                                 .order_id = 2,
                                 .side = Side::Sell,
                                 .price = Price{100'500},
                                 .quantity = Quantity{50},
                                 .liquidity = Liquidity::Taker,
                                 .rule = FillCause::RuleCReplace,
                                 .ahead_at_entry = 7,
                                 .position = 50}},
                      .truncated = true};
    results.fills_by[0][2] = 1;
    results.anomalies_by[3][1] = 2;
    results.orb = OrbStats{.long_entries = 1,
                           .stops = 1,
                           .closing_orders = 2,
                           .requested = 200,
                           .bought = 100,
                           .sold = 100,
                           .has_range = true,
                           .range_high = Price{100'500},
                           .range_low = Price{99'800}};
    RunSpec spec = default_spec(SessionStrategyKind::MarketMaker);
    spec.ticker = ticker("ACME");
    spec.days = {.day = {2019y / 12 / 30}, .count = 1};
    spec.latency = {.order_latency = 123'456, .report_latency = 7};
    spec.fees = {.maker_fee_per_share = -2, .taker_fee_per_share = 3};
    spec.limits.max_open_orders = 4;
    spec.window = TradingWindow::FullDay;
    spec.strategy.market_maker.improve_ticks = 2;
    server::Run run{.id = "20261018T101500Z-9c2e41d07ab35f18",
                    .state = RunState::Completed,
                    .warning = "not saved: No space left on device",
                    .spec = spec,
                    .provenance = {.build = {.commit = "abc1234",
                                             .dirty = true,
                                             .source_digest = std::string(64, 'f'),
                                             .compiler = "AppleClang 17.0.0",
                                             .build_type = "RelWithDebInfo",
                                             .sanitize = "address",
                                             .platform = "Darwin arm64"},
                                   .book_capacity = 1'048'576,
                                   .price_scale = 4,
                                   .quantity_scale = 2,
                                   .check_invariants_every = 1'000,
                                   .allow_truncated_tail = true,
                                   .days = {{.date = 2019y / 12 / 30,
                                             .file = "12302019.NASDAQ_ITCH50.bin",
                                             .size = 7,
                                             .hash = std::string(32, 'a'),
                                             .session = {.open = 1, .close = 2, .source = SessionSource::Feed}}},
                                   .host = "host",
                                   .submitted = "2026-10-18T10:15:00Z",
                                   .started = "2026-10-18T10:15:01Z",
                                   .finished = "2026-10-18T10:15:04Z",
                                   .wall_ms = 3,
                                   .cpu_ms = 2},
                    .results = std::make_shared<const RunResult>(results)};
    const json written = json::parse(encode_run(run));
    EXPECT_EQ(written["format"], 1);
    EXPECT_EQ(written["state"], "completed");
    EXPECT_EQ(written["results"]["digests"]["book"], "cbf29ce484222325");
    EXPECT_EQ(written["results"]["fills"], 2);
    EXPECT_EQ(written["results"]["orb"]["range_high"], 100'500);
    EXPECT_EQ(written["results"]["equity"]["t"], json::parse("[1577716201,1577716202]"));
    EXPECT_EQ(written["results"]["by_hour"], json::parse(R"({"hour":[9],"pnl":[-200]})"));
    EXPECT_EQ(written["results"]["attribution"]["fills"][0], json::parse("[0,0,1,0,0]"));
    EXPECT_EQ(written["results"]["fill_list"]["rows"][0],
              json::parse(R"([1577716201500000000,1,"buy",100000,100,"maker","a_e",300,100])"));
    EXPECT_EQ(written["results"]["fill_list"]["rows"][1],
              json::parse(R"([1577716202000000000,2,"sell",100500,50,"taker","c_replace",7,50])"));
    EXPECT_EQ(written["spec"]["window"], "full_day");
    EXPECT_EQ(written["reproduced"], nullptr);
    EXPECT_EQ(written["verifying"], nullptr);

    server::Run read;
    ASSERT_TRUE(decode_run(encode_run(run), read));
    EXPECT_EQ(encode_run(read), encode_run(run));
    EXPECT_EQ(*read.results, results);

    const std::string code = run.provenance.build.source_digest;
    const Verification matched{.finished = "2026-10-18T11:00:00Z",
                               .outcome = Outcome::Reproduced,
                               .digests = results.digests,
                               .source_digest = code};
    run.verifications = {{.finished = "2026-10-18T10:30:00Z",
                          .outcome = Outcome::DifferentCode,
                          .digests = Digests{.book = 7, .execution = 8, .portfolio = 9},
                          .source_digest = std::string(64, 'e')},
                         matched};
    EXPECT_EQ(json::parse(encode_run(run))["reproduced"], true);
    run.verifications.push_back({.finished = "2026-10-18T12:00:00Z",
                                 .outcome = Outcome::InputChanged,
                                 .source_digest = code,
                                 .changed = {2019y / 12 / 30}});
    EXPECT_EQ(json::parse(encode_run(run))["reproduced"], true);  // neither proves nor disproves it
    run.verifications.push_back(
        {.finished = "2026-10-18T13:00:00Z", .outcome = Outcome::Mismatch, .source_digest = code});
    const json verified = json::parse(encode_run(run));
    EXPECT_EQ(verified["reproduced"], false);
    EXPECT_EQ(verified["verifications"][2]["changed"], json::parse(R"(["2019-12-30"])"));
    EXPECT_EQ(verified["verifications"][3]["digests"], nullptr);
    ASSERT_TRUE(decode_run(encode_run(run), read));
    EXPECT_EQ(encode_run(read), encode_run(run));

    run.verifying = RunState::Running;  // written for the page; a restart drops it
    EXPECT_EQ(json::parse(encode_run(run))["verifying"], "running");
    ASSERT_TRUE(decode_run(encode_run(run), read));
    EXPECT_FALSE(read.verifying.has_value());
    run.verifying.reset();

    // A document with a digest that is not 16 hex digits, or a name that is none of its enum's, is refused: even the
    // one name that nlohmann would read as its enum's first.
    const auto corrupted = [&](const std::string& from, const std::string& to) {
        std::string doc = encode_run(run);
        const std::size_t at = doc.find(from);
        EXPECT_NE(at, std::string::npos) << from;
        return at == std::string::npos ? doc : doc.replace(at, from.size(), to);
    };
    EXPECT_FALSE(decode_run(corrupted("cbf29ce484222325", "cbf29ce48422232g"), read));
    EXPECT_FALSE(decode_run(corrupted("cbf29ce484222325", "cbf29ce48422232"), read));
    EXPECT_FALSE(decode_run(corrupted(R"("state":"completed")", R"("state":"finished")"), read));
    EXPECT_FALSE(decode_run(corrupted(R"("outcome":"different_code")", R"("outcome":"unknown")"), read));

    // Failed and cancelled runs keep what they had counted, without digests.
    RunResult partial = results;
    partial.partial = true;
    partial.digests = {};
    run.results = std::make_shared<const RunResult>(partial);
    run.state = RunState::Failed;
    run.reason = "source_error on 2019-12-30: TruncatedMessage";
    run.progress = 0.25;
    EXPECT_EQ(json::parse(encode_run(run))["results"]["digests"], nullptr);
    ASSERT_TRUE(decode_run(encode_run(run), read));
    EXPECT_EQ(read.state, RunState::Failed);
    EXPECT_EQ(read.reason, run.reason);
    EXPECT_EQ(read.warning, run.warning);
    EXPECT_EQ(*read.results, partial);
    EXPECT_EQ(encode_run(read), encode_run(run));
    run.state = RunState::Cancelled;
    run.reason.clear();
    ASSERT_TRUE(decode_run(encode_run(run), read));
    EXPECT_EQ(read.state, RunState::Cancelled);
    EXPECT_EQ(*read.results, partial);
    EXPECT_EQ(encode_run(read), encode_run(run));

    const json listed = json::parse(encode_runs(std::span{&run, 1}, 3));
    EXPECT_FALSE(listed["runs"][0].contains("results"));
    EXPECT_EQ(listed["runs"][0]["progress"], 0.25);
    EXPECT_EQ(listed["runs"][0]["partial"], true);
    EXPECT_EQ(listed["runs"][0]["pnl"], -200);
    EXPECT_EQ(listed["runs"][0]["fills"], 2);
    EXPECT_EQ(listed["queued"], 3);
    EXPECT_EQ(listed["max_queued"], 8);

    std::string text = encode_run(run);
    EXPECT_FALSE(decode_run(text.replace(text.find("\"format\":1"), 10, "\"format\":2"), read));
    run.state = RunState::Running;
    EXPECT_FALSE(decode_run(encode_run(run), read));  // a run is saved only once it has finished
    EXPECT_FALSE(decode_run("{}", read));
}

// A Watch control names its run. generation_start and state carry the run id, and a Watch's verdict is
// report{kind:"reproduced"}: its outcome, its digests and the run's.
TEST(ProtocolTest, DecodesAWatchAndEncodesItsRunIdAndVerdict)
{
    const ClientMessage watch =
        decoded(client_message("control", R"(,"action":"watch","run_id":"20261018T101500Z-9c2e41d07ab35f18")"));
    EXPECT_EQ(watch.control.type, ControlType::Watch);
    EXPECT_EQ(name(watch.control.watch.run_id), "20261018T101500Z-9c2e41d07ab35f18");
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"watch")")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"watch","run_id":"")")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"watch","run_id":")" + std::string(41, 'x') + "\"")));
    EXPECT_TRUE(refused(client_message("control", R"(,"action":"watch","run_id":"r1","symbol":"SPY")")));

    Watched w{.expected = {.book = 1, .execution = 2, .portfolio = 3}};
    ASSERT_TRUE(to_padded("r1", w.run_id));
    const json start = json::parse(encode_record(
        "s1", Record{.seq = 3, .generation = 2, .kind = RecordKind::GenerationStart, .control = {.watch = w}}));
    EXPECT_EQ(start["run_id"], "r1");
    EXPECT_EQ(json::parse(encode_record("s1",
                                        Record{.seq = 9,
                                               .generation = 2,
                                               .kind = RecordKind::Verdict,
                                               .control = {.watch = w},
                                               .digests = {.book = 1, .execution = 2, .portfolio = 4},
                                               .outcome = Outcome::Mismatch})),
              json::parse(R"({"type":"report","session_id":"s1","generation":2,"seq":9,"kind":"reproduced",)"
                          R"("run_id":"r1","outcome":"mismatch","digests":{"book":"0000000000000001",)"
                          R"("execution":"0000000000000002","portfolio":"0000000000000004"},)"
                          R"("expected":{"book":"0000000000000001","execution":"0000000000000002",)"
                          R"("portfolio":"0000000000000003"}})"));
    Snapshot s;
    s.run_id = w.run_id;
    EXPECT_EQ(json::parse(encode_snapshot("s1", s, true, false))["run_id"], "r1");
}

}  // namespace
}  // namespace simulator::server::test
