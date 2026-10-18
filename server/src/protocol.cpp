#include "chronos/server/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

#include <nlohmann/json.hpp>

#include "json_enums.hpp"

namespace simulator::server {
namespace {

using nlohmann::json;

constexpr std::uint64_t int64_max = std::numeric_limits<std::int64_t>::max();
// Strategy params, capped as the CLI caps them (src/cli/replay_args.cpp), so the strategies' signed arithmetic on
// them (sizes, positions and ticks added to positions and prices; a cooldown added to a timestamp) cannot overflow.
constexpr std::uint64_t max_param = 1'000'000'000;
constexpr std::uint64_t max_cooldown_ns = 1'000'000'000'000'000;  // 1e9 ms
// The longest part of a client's `type` an error echoes.
constexpr std::size_t max_echoed_type = 32;

// True if every key of `object` is one of `keys`.
bool only(const json& object, std::initializer_list<std::string_view> keys)
{
    for (const auto& item : object.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
            return false;
        }
    }
    return true;
}

// Reads `key` as an unsigned integer of at most `max`.
bool read(const json& object, const char* key, std::uint64_t max, std::uint64_t& value)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned() || it->get<std::uint64_t>() > max) {
        return false;
    }
    value = it->get<std::uint64_t>();
    return true;
}

bool read(const json& object, const char* key, std::string& value)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return false;
    }
    value = it->get<std::string>();
    return true;
}

// Reads `key` as one of E's names; an unknown name, which nlohmann reads as the first enumerator, is refused.
template <class E>
bool read(const json& object, const char* key, E& value)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return false;
    }
    value = it->get<E>();
    return json(value) == *it;
}

// An absent `key` keeps `value`; a present one must be a non-negative integer of at most `max`.
template <class I>
bool read_optional(const json& object, const char* key, std::uint64_t max, I& value)
{
    std::uint64_t raw = 0;
    if (!object.contains(key)) {
        return true;
    }
    if (!read(object, key, max, raw)) {
        return false;
    }
    value = static_cast<I>(raw);
    return true;
}

bool decode_control(const json& j, Control& control)
{
    std::uint64_t value = 0;
    std::string symbol;
    if (!read(j, "action", control.type)) {
        return false;
    }
    switch (control.type) {
    case ControlType::Step:
        return only(j, {"type", "session_id", "generation", "action", "n"}) && read(j, "n", int64_max, control.steps) &&
            control.steps > 0;
    case ControlType::Speed:
        if (!only(j, {"type", "session_id", "generation", "action", "speed"}) ||
            !read(j, "speed", ReplaySession::max_speed, value)) {
            return false;
        }
        control.speed = static_cast<std::uint32_t>(value);
        return true;
    case ControlType::Seek:
        if (!only(j, {"type", "session_id", "generation", "action", "ts"}) || !read(j, "ts", int64_max, value)) {
            return false;
        }
        control.seek_ts = static_cast<Timestamp>(value);
        return true;
    case ControlType::Symbol:
        return only(j, {"type", "session_id", "generation", "action", "symbol"}) && read(j, "symbol", symbol) &&
            !symbol.empty() &&
            std::all_of(symbol.begin(), symbol.end(), [](char c) { return c >= 0x21 && c <= 0x7E; }) &&
            to_ticker(symbol, control.symbol);
    case ControlType::Play:
    case ControlType::Pause:
    case ControlType::Restart:
    case ControlType::CancelSeek:
        break;
    }
    return only(j, {"type", "session_id", "generation", "action"});
}

bool decode_order(const json& j, Command& c)
{
    std::uint64_t quantity = 0;
    std::uint64_t price = 0;
    c.type = CommandType::Order;
    if (!only(j, {"type", "session_id", "generation", "client_id", "side", "order_type", "price", "qty"}) ||
        !read(j, "client_id", std::numeric_limits<std::uint64_t>::max(), c.client_id) || !read(j, "side", c.side) ||
        !read(j, "order_type", c.order_type) || !read(j, "qty", int64_max, quantity) || quantity == 0) {
        return false;
    }
    c.quantity = Quantity{quantity};
    if (c.order_type == OrderType::Market) {
        return !j.contains("price");  // its limit is the band around the best opposite price
    }
    if (!read(j, "price", int64_max, price) || price == 0) {
        return false;
    }
    c.price = Price{static_cast<std::int64_t>(price)};
    return true;
}

// The strategy and its params; a param left out keeps its default.
bool decode_attach(const json& j, Command& c)
{
    c.type = CommandType::Attach;
    StrategySpec& spec = c.strategy;
    if (!only(j, {"type", "session_id", "generation", "strategy", "params"}) || !read(j, "strategy", spec.kind) ||
        spec.kind == SessionStrategyKind::None) {
        return false;
    }
    // By reference: copying a client's subtree recurses as deep as the client nested it.
    static const json no_params = json::object();
    const auto found = j.find("params");
    const json& params = found == j.end() ? no_params : *found;
    if (!params.is_object()) {
        return false;
    }
    if (spec.kind == SessionStrategyKind::MarketMaker) {
        MarketMakerParams& mm = spec.market_maker;
        return only(params, {"quote_size", "max_position", "requote_cooldown_ns", "improve_ticks"}) &&
            read_optional(params, "quote_size", max_param, mm.quote_size) && mm.quote_size > 0 &&
            read_optional(params, "max_position", max_param, mm.max_position) &&
            read_optional(params, "requote_cooldown_ns", max_cooldown_ns, mm.requote_cooldown_ns) &&
            read_optional(params, "improve_ticks", max_param, mm.improve_ticks);
    }
    ImbalanceTakerParams& it = spec.imbalance;
    if (params.contains("threshold")) {
        const json& threshold = params.at("threshold");
        if (!threshold.is_number() || !std::isfinite(threshold.get<double>()) || threshold.get<double>() < 1.0) {
            return false;
        }
        it.threshold = threshold.get<double>();
    }
    return only(params, {"threshold", "clip", "max_position", "cooldown_ns"}) &&
        read_optional(params, "clip", max_param, it.clip) && it.clip > 0 &&
        read_optional(params, "max_position", max_param, it.max_position) &&
        read_optional(params, "cooldown_ns", max_cooldown_ns, it.cooldown_ns);
}

std::string text(const json& j)
{
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

json envelope(std::string_view type, std::string_view session_id, std::uint64_t generation)
{
    return {{"type", type}, {"session_id", session_id}, {"generation", generation}};
}

json level_json(const LevelInfo& level)
{
    return json::array({level.price.ticks, level.quantity, level.order_count});
}

json trade_json(const Trade& trade)
{
    return json::array({trade.timestamp, trade.price.ticks, trade.quantity, trade.side});
}

json candle_json(const Candle& bar)
{
    return json::array({bar.start, bar.open.ticks, bar.high.ticks, bar.low.ticks, bar.close.ticks, bar.volume});
}

json order_json(const MyOrder& order)
{
    return {{"id", order.id},
            {"side", order.side},
            {"price", order.price.ticks},
            {"remaining", order.remaining},
            {"ahead_quantity", order.ahead_quantity},
            {"ahead_orders", order.ahead_orders}};
}

template <class T, class F>
json array_of(std::span<const T> items, F&& item_json)
{
    json array = json::array();
    for (const T& item : items) {
        array.push_back(item_json(item));
    }
    return array;
}

}  // namespace

bool decode(std::string_view text, ClientMessage& message, std::string& error)
{
    message = ClientMessage{};
    if (text.size() > max_message_bytes) {
        error = "over 64 KiB";
        return false;
    }
    const json j = json::parse(text, nullptr, false);
    std::string type;
    if (!j.is_object() || !read(j, "type", type)) {
        error = "not a JSON object with a type";
        return false;
    }
    if (type == "auth") {
        message.type = ClientType::Auth;
        error = "auth{token}";
        return only(j, {"type", "token"}) && read(j, "token", message.token);
    }
    if (!read(j, "session_id", message.session_id) ||
        !read(j, "generation", std::numeric_limits<std::uint64_t>::max(), message.generation)) {
        error = "no session_id and generation";
        return false;
    }
    Command& command = message.command;
    command.generation = message.generation;
    message.type = ClientType::Command;
    const std::string shown = type.substr(0, max_echoed_type);
    error = shown + ": a key is missing, unknown, of the wrong type or out of range";
    if (type == "resync") {
        message.type = ClientType::Resync;
        return only(j, {"type", "session_id", "generation", "from_report_seq"}) &&
            read(j, "from_report_seq", std::numeric_limits<std::uint64_t>::max(), message.from_report_seq);
    }
    if (type == "control") {
        message.type = ClientType::Control;
        return decode_control(j, message.control);
    }
    if (type == "order") {
        std::uint64_t client_id = 0;
        if (read(j, "client_id", std::numeric_limits<std::uint64_t>::max(), client_id)) {
            message.client_id = client_id;
        }
        return decode_order(j, command);
    }
    if (type == "cancel") {
        command.type = CommandType::Cancel;
        return only(j, {"type", "session_id", "generation", "order_id"}) &&
            read(j, "order_id", std::numeric_limits<std::uint64_t>::max(), command.order_id);
    }
    if (type == "attach") {
        return decode_attach(j, command);
    }
    if (type == "detach") {
        command.type = CommandType::Detach;
        return only(j, {"type", "session_id", "generation"});
    }
    error = "unknown type " + shown;
    return false;
}

std::string encode_hello(std::string_view session_id, std::uint64_t generation, bool controller,
                         const SessionConfig& config)
{
    json j = envelope("hello", session_id, generation);
    j["protocol"] = ws_protocol;
    j["role"] = controller ? "controller" : "viewer";
    j["price_scale"] = config.instrument.price_scale;
    j["quantity_scale"] = config.instrument.quantity_scale;
    j["fees"] = {{"maker", config.fees.maker_fee_per_share}, {"taker", config.fees.taker_fee_per_share}};
    j["limits"] = {{"max_position", config.limits.max_position},
                   {"max_order_quantity", config.limits.max_order_quantity.value()},
                   {"max_open_orders", config.limits.max_open_orders}};
    return text(j);
}

std::string encode_snapshot(std::string_view session_id, const Snapshot& s, bool state, bool full_resync)
{
    json j = envelope(state ? "state" : "snapshot", session_id, s.generation);
    if (state) {
        j["full_resync"] = full_resync;
    }
    j["symbol"] = name(s.symbol);
    j["snapshot_seq"] = s.snapshot_seq;
    j["exchange_ts"] = s.exchange_ts;
    j["event_seq"] = s.event_seq;
    j["last_report_seq"] = s.last_report_seq;
    j["last_command_seq"] = s.last_command_seq;
    j["state"] = s.state;
    j["speed"] = s.speed;
    j["progress"] = s.progress;
    j["failure"] = s.failure;
    j["source_error"] = to_string(s.source_error);
    j["trading_state"] = s.trading_state;
    j["bids"] = array_of(std::span{s.bids}.first(s.bid_levels), level_json);
    j["asks"] = array_of(std::span{s.asks}.first(s.ask_levels), level_json);
    j["trades"] = array_of(std::span{s.trades}.first(s.trade_count), trade_json);
    j["trades_truncated"] = s.trades_truncated;
    j["candle"] = s.has_candle ? candle_json(s.candle) : json(nullptr);
    j["closed_candles"] = array_of(std::span{s.closed_candles}.first(s.closed_count), candle_json);
    j["candles_truncated"] = s.candles_truncated;
    const Account& a = s.account;
    j["account"] = {{"position", a.position},        {"cash", a.cash}, {"realised", a.realised},
                    {"unrealised", a.unrealised},    {"fees", a.fees}, {"pnl", a.pnl},
                    {"max_drawdown", a.max_drawdown}};
    j["my_orders"] = array_of(std::span{s.my_orders}.first(s.my_order_count), order_json);
    j["my_orders_truncated"] = s.my_orders_truncated;
    const SnapshotMetrics& m = s.metrics;
    j["metrics"] = {{"events_per_second", m.events_per_second},
                    {"dropped_snapshots", m.dropped_snapshots},
                    {"command_queue_depth", m.command_queue_depth},
                    {"log_failed", m.log_failed}};
    return text(j);
}

std::string encode_record(std::string_view session_id, const Record& r)
{
    if (r.kind == RecordKind::Ack) {
        json j = envelope("ack", session_id, r.generation);
        j["seq"] = r.seq;
        j["control"] = r.control.type;
        j["control_seq"] = r.control.control_seq;
        j["state"] = r.state;
        return text(j);
    }
    json j = envelope("report", session_id, r.generation);
    j["seq"] = r.seq;
    switch (r.kind) {
    case RecordKind::Lifecycle:
        j["kind"] = "lifecycle";
        j["step"] = r.step;
        j["reason"] = to_string(r.reason);
        j["command"] = r.command.type;
        j["command_seq"] = r.command.command_seq;
        j["client_id"] = r.command.client_id;
        j["order_id"] = r.order_id;
        j["exchange_ts"] = r.exchange_ts;
        j["market_event_seq"] = r.market_event_seq;
        break;
    case RecordKind::Report:
        j["kind"] = "execution";
        j["order_id"] = r.report.order_id;
        j["report_type"] = r.report.type;
        j["side"] = r.report.side;
        j["price"] = r.report.price.ticks;
        j["quantity"] = r.report.quantity.value();
        j["leaves"] = r.report.leaves.value();
        j["reject_reason"] = r.report.reason;
        j["liquidity"] = r.report.liquidity;
        j["exchange_ts"] = r.report.exchange_time;
        if (r.report.liquidity == Liquidity::Maker) {
            j["rule"] = r.report.cause;
            j["ahead_at_entry"] = r.report.entry_ahead_quantity;
        }
        break;
    case RecordKind::GenerationStart:
        j["kind"] = "generation_start";
        j["symbol"] = name(r.control.symbol);
        j["strategy"] = r.command.strategy.kind;
        break;
    case RecordKind::GenerationEnd:
        j["kind"] = "generation_end";
        j["exchange_ts"] = r.exchange_ts;
        j["market_event_seq"] = r.market_event_seq;
        break;
    case RecordKind::Ack:
        break;
    }
    return text(j);
}

std::string encode_command_ack(std::string_view session_id, std::uint64_t generation, std::uint64_t command_seq,
                               bool fsynced)
{
    json j = envelope("ack", session_id, generation);
    j["command_seq"] = command_seq;
    j["persisted"] = fsynced ? "fsynced" : "flushed";
    return text(j);
}

std::string encode_gap(std::string_view session_id, std::uint64_t generation, std::uint64_t from, std::uint64_t to)
{
    json j = envelope("gap", session_id, generation);
    j["from"] = from;
    j["to"] = to;
    return text(j);
}

std::string encode_progress(std::string_view session_id, std::uint64_t generation, Timestamp seek)
{
    json j = envelope("progress", session_id, generation);
    j["seek"] = seek;
    return text(j);
}

std::string encode_error(std::string_view session_id, std::uint64_t generation, std::string_view code,
                         std::string_view message, std::uint64_t command_seq, std::optional<std::uint64_t> client_id)
{
    json j = envelope("error", session_id, generation);
    j["code"] = code;
    j["message"] = message;
    if (command_seq != 0) {
        j["command_seq"] = command_seq;
    }
    if (client_id) {
        j["client_id"] = *client_id;
    }
    return text(j);
}

std::string encode_symbols(std::span<const std::string> symbols)
{
    return text({{"symbols", symbols}});
}

std::string encode_history(std::string_view session_id, std::uint64_t generation, std::span<const Candle> candles,
                           std::span<const Trade> trades)
{
    return text({{"session_id", session_id},
                 {"generation", generation},
                 {"candles", array_of(candles, candle_json)},
                 {"trades", array_of(trades, trade_json)}});
}

}  // namespace simulator::server
