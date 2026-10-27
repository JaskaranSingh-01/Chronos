#include "chronos/server/protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "json_enums.hpp"
#include "simulator/cli/replay_args.hpp"

namespace simulator::server {
namespace {

using nlohmann::json;

constexpr std::uint64_t int64_max = std::numeric_limits<std::int64_t>::max();
// Strategy params, capped as the CLI caps them (src/cli/replay_args.cpp), so the strategies' signed arithmetic on
// them (sizes, positions and ticks added to positions and prices; a cooldown added to a timestamp) cannot overflow.
constexpr std::uint64_t max_param = 1'000'000'000;
constexpr std::uint64_t max_cooldown_ns = 1'000'000'000'000'000;  // 1e9 ms
constexpr std::uint64_t max_range_minutes = 390;                    // the regular session
// A Lab run's latencies and fees, capped as the CLI caps them.
constexpr std::uint64_t max_latency_ns = 1'000'000'000'000;  // 1,000 s
constexpr std::int64_t max_fee = 1'000'000;                  // ticks a share
// The longest part of a client's `type` an error echoes.
constexpr std::size_t max_echoed_type = 32;

// The first key of `object` that is not one of `keys`, which may itself be ""; none if every key is one of them.
std::optional<std::string> unknown_key(const json& object, std::initializer_list<std::string_view> keys)
{
    for (const auto& item : object.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
            return item.key();
        }
    }
    return std::nullopt;
}

// True if every key of `object` is one of `keys`.
bool only(const json& object, std::initializer_list<std::string_view> keys)
{
    return !unknown_key(object, keys);
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

// An absent `key` keeps `value`; a present one must be an integer from `min` to `max`.
bool read_signed(const json& object, const char* key, std::int64_t min, std::int64_t max, std::int64_t& value)
{
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_number_integer() || (it->is_number_unsigned() && it->get<std::uint64_t>() > int64_max) ||
        it->get<std::int64_t>() < min || it->get<std::int64_t>() > max) {
        return false;
    }
    value = it->get<std::int64_t>();
    return true;
}

// An optional "days": 1 to max_days dates, YYYY-MM-DD, strictly increasing; absent, the count stays 0. False with
// `field` naming "days", or the first bad date as "days[i]".
bool read_days(const json& j, DayList& days, std::string& field)
{
    field = "days";
    const auto it = j.find("days");
    if (it == j.end()) {
        return true;
    }
    if (!it->is_array() || it->empty() || it->size() > max_days) {
        return false;
    }
    for (const json& day : *it) {
        field = "days[" + std::to_string(days.count) + "]";
        std::chrono::year_month_day& date = days.day[days.count];
        if (!day.is_string() || !parse_date(day.get<std::string>(), date) ||
            (days.count > 0 && date <= days.day[days.count - 1])) {
            return false;
        }
        ++days.count;
    }
    return true;
}

// "symbol": 1 to 8 printable ASCII characters, an ITCH ticker.
bool read_symbol(const json& j, Ticker& ticker)
{
    std::string symbol;
    return read(j, "symbol", symbol) && !symbol.empty() &&
        std::all_of(symbol.begin(), symbol.end(), [](char c) { return c >= 0x21 && c <= 0x7E; }) &&
        to_ticker(symbol, ticker);
}

bool decode_control(const json& j, Control& control)
{
    std::uint64_t value = 0;
    std::string field;
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
        return only(j, {"type", "session_id", "generation", "action", "symbol", "days"}) &&
            read_symbol(j, control.spec.ticker) && read_days(j, control.spec.days, field);
    case ControlType::Watch:
        return only(j, {"type", "session_id", "generation", "action", "run_id"}) && read(j, "run_id", field) &&
            !field.empty() && to_padded(field, control.watch.run_id);
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

// The strategy and its params, as attach and a Lab run take them: a param left out keeps its default. False with
// `field` naming "strategy", "params" or the first bad param, e.g. "params.quote_size".
bool decode_strategy(const json& j, StrategySpec& spec, std::string& field)
{
    field = "strategy";
    if (!read(j, "strategy", spec.kind) || spec.kind == SessionStrategyKind::None) {
        return false;
    }
    // By reference: copying a client's subtree recurses as deep as the client nested it.
    static const json no_params = json::object();
    const auto found = j.find("params");
    const json& params = found == j.end() ? no_params : *found;
    field = "params";
    if (!params.is_object()) {
        return false;
    }
    const auto bad = [&](std::string_view param) {
        field = "params." + std::string{param};
        return false;
    };
    if (spec.kind == SessionStrategyKind::MarketMaker) {
        MarketMakerParams& mm = spec.market_maker;
        if (const auto key =
                unknown_key(params, {"quote_size", "max_position", "requote_cooldown_ns", "improve_ticks"})) {
            return bad(*key);
        }
        if (!read_optional(params, "quote_size", max_param, mm.quote_size) || mm.quote_size == 0) {
            return bad("quote_size");
        }
        if (!read_optional(params, "max_position", max_param, mm.max_position)) {
            return bad("max_position");
        }
        if (!read_optional(params, "requote_cooldown_ns", max_cooldown_ns, mm.requote_cooldown_ns)) {
            return bad("requote_cooldown_ns");
        }
        return read_optional(params, "improve_ticks", max_param, mm.improve_ticks) || bad("improve_ticks");
    }
    if (spec.kind == SessionStrategyKind::Orb) {
        OrbParams& orb = spec.orb;
        if (const auto key = unknown_key(params, {"range_minutes", "clip"})) {
            return bad(*key);
        }
        if (!read_optional(params, "range_minutes", max_range_minutes, orb.range_minutes) || orb.range_minutes == 0) {
            return bad("range_minutes");
        }
        return (read_optional(params, "clip", max_param, orb.clip) && orb.clip != 0) || bad("clip");
    }
    ImbalanceTakerParams& it = spec.imbalance;
    if (const auto key = unknown_key(params, {"threshold", "clip", "max_position", "cooldown_ns"})) {
        return bad(*key);
    }
    if (params.contains("threshold")) {
        const json& threshold = params.at("threshold");
        if (!threshold.is_number() || !std::isfinite(threshold.get<double>()) || threshold.get<double>() < 1.0) {
            return bad("threshold");
        }
        it.threshold = threshold.get<double>();
    }
    if (!read_optional(params, "clip", max_param, it.clip) || it.clip == 0) {
        return bad("clip");
    }
    if (!read_optional(params, "max_position", max_param, it.max_position)) {
        return bad("max_position");
    }
    return read_optional(params, "cooldown_ns", max_cooldown_ns, it.cooldown_ns) || bad("cooldown_ns");
}

bool decode_attach(const json& j, Command& c)
{
    c.type = CommandType::Attach;
    std::string field;
    return only(j, {"type", "session_id", "generation", "strategy", "params"}) && decode_strategy(j, c.strategy, field);
}

// The optional object `key` of `j`, or an empty one; nullptr, naming `field`, if it is not an object or holds a key not
// in `keys`.
const json* section(const json& j, const char* key, std::initializer_list<std::string_view> keys, std::string& field)
{
    static const json empty = json::object();
    const auto it = j.find(key);
    if (it == j.end()) {
        return &empty;
    }
    field = key;
    if (!it->is_object()) {
        return nullptr;
    }
    if (const auto unknown = unknown_key(*it, keys)) {
        field += "." + *unknown;
        return nullptr;
    }
    return &*it;
}

// A Lab run's RunSpec; see decode_run_spec().
bool spec_from(const json& j, RunSpec& spec, std::string& field)
{
    if (const auto key =
            unknown_key(j, {"symbol", "days", "strategy", "params", "latency", "fees", "limits", "window"})) {
        field = *key;
        return false;
    }
    StrategySpec strategy;
    if (!decode_strategy(j, strategy, field)) {
        return false;
    }
    spec = default_spec(strategy.kind);
    spec.strategy = strategy;
    spec.limits = default_limits(strategy);
    field = "symbol";
    if (!read_symbol(j, spec.ticker)) {
        return false;
    }
    if (!read_days(j, spec.days, field) || spec.days.count == 0) {
        return false;
    }
    const auto bad = [&](const char* name) {
        field = name;
        return false;
    };
    const json* latency = section(j, "latency", {"order_ns", "report_ns"}, field);
    if (latency == nullptr) {
        return false;
    }
    if (!read_optional(*latency, "order_ns", max_latency_ns, spec.latency.order_latency)) {
        return bad("latency.order_ns");
    }
    if (!read_optional(*latency, "report_ns", max_latency_ns, spec.latency.report_latency)) {
        return bad("latency.report_ns");
    }
    const json* fees = section(j, "fees", {"maker", "taker"}, field);
    if (fees == nullptr) {
        return false;
    }
    if (!read_signed(*fees, "maker", -max_fee, max_fee, spec.fees.maker_fee_per_share)) {
        return bad("fees.maker");
    }
    if (!read_signed(*fees, "taker", 0, max_fee, spec.fees.taker_fee_per_share)) {
        return bad("fees.taker");
    }
    const json* limits = section(j, "limits", {"max_position", "max_order_quantity", "max_open_orders"}, field);
    if (limits == nullptr) {
        return false;
    }
    std::uint64_t order_quantity = spec.limits.max_order_quantity.value();
    if (!read_optional(*limits, "max_position", max_param, spec.limits.max_position)) {
        return bad("limits.max_position");
    }
    if (!read_optional(*limits, "max_order_quantity", max_param, order_quantity) || order_quantity == 0) {
        return bad("limits.max_order_quantity");
    }
    spec.limits.max_order_quantity = Quantity{order_quantity};
    if (!read_optional(*limits, "max_open_orders", max_param, spec.limits.max_open_orders) ||
        spec.limits.max_open_orders == 0) {
        return bad("limits.max_open_orders");
    }
    if (strategy.kind == SessionStrategyKind::Orb &&
        (strategy.orb.clip > static_cast<std::uint64_t>(spec.limits.max_position) ||
         strategy.orb.clip > spec.limits.max_order_quantity.value())) {
        return bad("params.clip");  // the ORB could never enter
    }
    return !j.contains("window") || read(j, "window", spec.window) || bad("window");
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

json days_json(const DayList& days)
{
    json dates = json::array();
    for (std::size_t i = 0; i < days.count; ++i) {
        dates.push_back(iso_date(days.day[i]));
    }
    return dates;
}

// A generation's settings in the shape a Lab run is posted in; no strategy has no params.
json fees_json(const RunSpec& spec)
{
    return {{"maker", spec.fees.maker_fee_per_share}, {"taker", spec.fees.taker_fee_per_share}};
}

json limits_json(const RunSpec& spec)
{
    return {{"max_position", spec.limits.max_position},
            {"max_order_quantity", spec.limits.max_order_quantity.value()},
            {"max_open_orders", spec.limits.max_open_orders}};
}

json spec_json(const RunSpec& spec)
{
    const MarketMakerParams& mm = spec.strategy.market_maker;
    const ImbalanceTakerParams& it = spec.strategy.imbalance;
    json params = json::object();
    if (spec.strategy.kind == SessionStrategyKind::MarketMaker) {
        params = {{"quote_size", mm.quote_size},
                  {"max_position", mm.max_position},
                  {"requote_cooldown_ns", mm.requote_cooldown_ns},
                  {"improve_ticks", mm.improve_ticks}};
    } else if (spec.strategy.kind == SessionStrategyKind::ImbalanceTaker) {
        params = {{"threshold", it.threshold},
                  {"clip", it.clip},
                  {"max_position", it.max_position},
                  {"cooldown_ns", it.cooldown_ns}};
    } else if (spec.strategy.kind == SessionStrategyKind::Orb) {
        params = {{"range_minutes", spec.strategy.orb.range_minutes}, {"clip", spec.strategy.orb.clip}};
    }
    return {{"symbol", name(spec.ticker)},
            {"days", days_json(spec.days)},
            {"strategy", spec.strategy.kind},
            {"params", params},
            {"latency", {{"order_ns", spec.latency.order_latency}, {"report_ns", spec.latency.report_latency}}},
            {"fees", fees_json(spec)},
            {"limits", limits_json(spec)},
            {"window", spec.window}};
}

// A source error, and in a generation of several days the day it failed on: the last whose midnight the clock reached.
std::string source_error(const Snapshot& s)
{
    std::string text{to_string(s.source_error)};
    const DayList& days = s.spec.days;
    if (s.source_error == SourceError::None || days.count < 2) {
        return text;
    }
    std::size_t day = 0;
    while (day + 1 < days.count &&
           session_midnight_ns(days.day[day + 1], us_eastern_utc_offset(days.day[day + 1])) <= s.exchange_ts) {
        ++day;
    }
    return text + " on " + iso_date(days.day[day]);
}

// 16 hex digits: a quick check or a digest, which exceed 2^53.
std::string hex16(std::uint64_t value)
{
    char text[17];
    std::snprintf(text, sizeof text, "%016" PRIx64, value);
    return text;
}

std::uint64_t hex16_from(const json& j)
{
    const std::string text = j.get<std::string>();
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (text.size() != 16 || error != std::errc{} || end != text.data() + text.size()) {
        throw std::invalid_argument{"not 16 hex digits"};
    }
    return value;
}

// One of E's names; an unknown one throws.
template <class E>
E name_from(const json& j)
{
    const E value = j.get<E>();
    if (json(value) != j) {
        throw std::invalid_argument{"an unknown name"};
    }
    return value;
}

std::chrono::year_month_day date_from(const json& j)
{
    std::chrono::year_month_day date{};
    if (!parse_date(j.get<std::string>(), date)) {
        throw std::invalid_argument{"not a date"};
    }
    return date;
}

// A day of /api/days; `symbols` counts its symbols with a market event.
json day_json(const DayHandle& day)
{
    return {{"date", iso_date(day->date)},
            {"file", day->file},
            {"size", day->size},
            {"hash", day->hash},
            {"quick_check", hex16(day->quick_check)},
            {"itch50", day->itch50},
            {"cut_tail", day->cut_tail},
            {"session", day->session},
            {"symbols", day->symbols.size()}};
}

json digests_json(const Digests& d)
{
    return {{"book", hex16(d.book)}, {"execution", hex16(d.execution)}, {"portfolio", hex16(d.portfolio)}};
}

Digests digests_from(const json& j)
{
    return {.book = hex16_from(j.at("book")),
            .execution = hex16_from(j.at("execution")),
            .portfolio = hex16_from(j.at("portfolio"))};
}

// Not written: RunResult's cancelled, reason, wall_ms and cpu_ms (the run's state, reason, provenance) and hashes.
json results_json(const RunResult& r)
{
    json t = json::array();
    json pnl = json::array();
    json position = json::array();
    for (const Sample& s : r.equity) {
        t.push_back(s.second);
        pnl.push_back(s.pnl);
        position.push_back(s.position);
    }
    json hours = json::array();
    json by_hour = json::array();
    for (const auto& [hour, change] : r.by_hour) {
        hours.push_back(hour);
        by_hour.push_back(change);
    }
    json by_day = json::array();
    for (const DayResult& day : r.by_day) {
        by_day.push_back({{"date", iso_date(day.date)}, {"pnl", day.pnl}, {"fills", day.fills}});
    }
    json rows = json::array();
    for (const FillRow& f : r.fills) {
        rows.push_back(json::array({f.exchange_ts, f.order_id, f.side, f.price.ticks, f.quantity.value(), f.liquidity,
                                    f.rule, f.ahead_at_entry, f.position}));
    }
    json results = {{"partial", r.partial},
                    {"events", r.events},
                    {"sim_events", r.sim_events},
                    {"pnl", r.pnl},
                    {"realised", r.realised},
                    {"unrealised", r.unrealised},
                    {"fees", r.fees},
                    {"position", r.position},
                    {"max_abs_position", r.max_abs_position},
                    {"max_drawdown", r.max_drawdown},
                    {"fills", r.maker_fills + r.taker_fills},
                    {"maker_fills", r.maker_fills},
                    {"taker_fills", r.taker_fills},
                    {"filled_shares", r.filled_shares},
                    {"fill_rate", r.fill_rate},
                    {"digests", r.partial ? json(nullptr) : digests_json(r.digests)},
                    {"equity", {{"granularity", "event_second"}, {"t", t}, {"pnl", pnl}, {"position", position}}},
                    {"by_hour", {{"hour", hours}, {"pnl", by_hour}}},
                    {"by_day", by_day},
                    {"attribution",
                     {{"rules", {"a_e", "a_c", "d_e", "d_c", "c_add", "c_replace"}},
                      {"phases", {"pre_open", "opening_cross", "regular", "closing_cross", "post_close"}},
                      {"fills", r.fills_by},
                      {"anomalies", r.anomalies_by}}},
                    {"fill_list", {{"truncated", r.truncated}, {"rows", rows}}}};
    if (r.orb) {
        const OrbStats& s = *r.orb;
        results["orb"] = {{"long_entries", s.long_entries},
                          {"short_entries", s.short_entries},
                          {"stops", s.stops},
                          {"flattens", s.flattens},
                          {"closing_orders", s.closing_orders},
                          {"risk_rejects", s.risk_rejects},
                          {"no_range_days", s.no_range_days},
                          {"requested", s.requested},
                          {"bought", s.bought},
                          {"sold", s.sold},
                          {"expired", s.expired},
                          {"range_high", s.has_range ? json(s.range_high.ticks) : json(nullptr)},
                          {"range_low", s.has_range ? json(s.range_low.ticks) : json(nullptr)}};
    }
    return results;
}

// A run's ORB stats, as results_json writes them.
OrbStats orb_stats_from(const json& j)
{
    OrbStats s{.long_entries = j.at("long_entries").get<std::uint64_t>(),
               .short_entries = j.at("short_entries").get<std::uint64_t>(),
               .stops = j.at("stops").get<std::uint64_t>(),
               .flattens = j.at("flattens").get<std::uint64_t>(),
               .closing_orders = j.at("closing_orders").get<std::uint64_t>(),
               .risk_rejects = j.at("risk_rejects").get<std::uint64_t>(),
               .no_range_days = j.at("no_range_days").get<std::uint64_t>(),
               .requested = j.at("requested").get<std::uint64_t>(),
               .bought = j.at("bought").get<std::uint64_t>(),
               .sold = j.at("sold").get<std::uint64_t>(),
               .expired = j.at("expired").get<std::uint64_t>(),
               .has_range = !j.at("range_high").is_null()};
    if (s.has_range) {
        s.range_high = Price{j.at("range_high").get<std::int64_t>()};
        s.range_low = Price{j.at("range_low").get<std::int64_t>()};
    }
    return s;
}

RunResult results_from(const json& j)
{
    RunResult r{.partial = j.at("partial").get<bool>(),
                .events = j.at("events").get<std::uint64_t>(),
                .sim_events = j.at("sim_events").get<std::uint64_t>(),
                .pnl = j.at("pnl").get<std::int64_t>(),
                .realised = j.at("realised").get<std::int64_t>(),
                .unrealised = j.at("unrealised").get<std::int64_t>(),
                .fees = j.at("fees").get<std::int64_t>(),
                .position = j.at("position").get<std::int64_t>(),
                .max_abs_position = j.at("max_abs_position").get<std::int64_t>(),
                .max_drawdown = j.at("max_drawdown").get<std::int64_t>(),
                .maker_fills = j.at("maker_fills").get<std::uint64_t>(),
                .taker_fills = j.at("taker_fills").get<std::uint64_t>(),
                .filled_shares = j.at("filled_shares").get<std::uint64_t>(),
                .fill_rate = j.at("fill_rate").get<double>(),
                .fills_by = j.at("attribution").at("fills").get<FillAttribution>(),
                .anomalies_by = j.at("attribution").at("anomalies").get<FillAttribution>(),
                .truncated = j.at("fill_list").at("truncated").get<bool>()};
    if (!r.partial) {
        r.digests = digests_from(j.at("digests"));
    }
    if (const auto orb = j.find("orb"); orb != j.end()) {  // a run before the ORB has none
        r.orb = orb_stats_from(*orb);
    }
    const json& equity = j.at("equity");
    for (std::size_t i = 0; i < equity.at("t").size(); ++i) {
        r.equity.push_back({.second = equity.at("t").at(i).get<std::int64_t>(),
                            .pnl = equity.at("pnl").at(i).get<std::int64_t>(),
                            .position = equity.at("position").at(i).get<std::int64_t>()});
    }
    const json& by_hour = j.at("by_hour");
    for (std::size_t i = 0; i < by_hour.at("hour").size(); ++i) {
        r.by_hour[by_hour.at("hour").at(i).get<int>()] = by_hour.at("pnl").at(i).get<std::int64_t>();
    }
    for (const json& day : j.at("by_day")) {
        r.by_day.push_back({.date = date_from(day.at("date")),
                            .pnl = day.at("pnl").get<std::int64_t>(),
                            .fills = day.at("fills").get<std::uint64_t>()});
    }
    for (const json& row : j.at("fill_list").at("rows")) {
        r.fills.push_back({.exchange_ts = row.at(0).get<Timestamp>(),
                           .order_id = row.at(1).get<OrderId>(),
                           .side = name_from<Side>(row.at(2)),
                           .price = Price{row.at(3).get<std::int64_t>()},
                           .quantity = Quantity{row.at(4).get<std::uint64_t>()},
                           .liquidity = name_from<Liquidity>(row.at(5)),
                           .rule = name_from<FillCause>(row.at(6)),
                           .ahead_at_entry = row.at(7).get<std::uint64_t>(),
                           .position = row.at(8).get<std::int64_t>()});
    }
    return r;
}

// True once a verification on the same code and inputs has run and every such one matched; null before.
json reproduced(const Run& run)
{
    json verdict = nullptr;
    for (const Verification& v : run.verifications) {
        if (v.outcome == Outcome::Reproduced || v.outcome == Outcome::Mismatch) {
            verdict = (verdict.is_null() || verdict.get<bool>()) && v.outcome == Outcome::Reproduced;
        }
    }
    return verdict;
}

// A run, with its results or, for a listing, without them.
json run_json(const Run& run, bool with_results)
{
    const Provenance& p = run.provenance;
    json days = json::array();
    for (const RunDay& day : p.days) {
        days.push_back({{"date", iso_date(day.date)},
                        {"file", day.file},
                        {"size", day.size},
                        {"hash", day.hash},
                        {"session", day.session},
                        {"segment",
                         day.segment_hash.empty() ? json(nullptr)
                                                  : json{{"hash", day.segment_hash}, {"size", day.segment_size}}}});
    }
    json verifications = json::array();
    for (const Verification& v : run.verifications) {
        json changed = json::array();
        for (const std::chrono::year_month_day date : v.changed) {
            changed.push_back(iso_date(date));
        }
        verifications.push_back({{"finished", v.finished},
                                 {"outcome", v.outcome},
                                 {"digests", v.digests ? digests_json(*v.digests) : json(nullptr)},
                                 {"source_digest", v.source_digest},
                                 {"changed", changed}});
    }
    json j = {{"format", 1},
              {"id", run.id},
              {"state", run.state},
              {"reason", run.reason},
              {"warning", run.warning},
              {"spec", spec_json(run.spec)},
              {"provenance",
               {{"build",
                 {{"commit", p.build.commit},
                  {"dirty", p.build.dirty},
                  {"source_digest", p.build.source_digest},
                  {"compiler", p.build.compiler},
                  {"build_type", p.build.build_type},
                  {"sanitize", p.build.sanitize},
                  {"platform", p.build.platform}}},
                {"engine",
                 {{"book_capacity", p.book_capacity},
                  {"price_scale", p.price_scale},
                  {"quantity_scale", p.quantity_scale},
                  {"check_invariants_every", p.check_invariants_every}}},
                {"allow_truncated_tail", p.allow_truncated_tail},
                {"days", days},
                {"host", p.host},
                {"submitted", p.submitted},
                {"started", p.started},
                {"finished", p.finished},
                {"wall_ms", p.wall_ms},
                {"cpu_ms", p.cpu_ms}}},
              {"verifying", run.verifying ? json(*run.verifying) : json(nullptr)},
              {"verifications", verifications},
              {"reproduced", reproduced(run)}};
    if (with_results) {
        j["results"] = run.results ? results_json(*run.results) : json(nullptr);
    }
    return j;
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
    const RunSpec& spec = config.defaults;
    j["fees"] = fees_json(spec);
    j["limits"] = limits_json(spec);
    return text(j);
}

std::string encode_snapshot(std::string_view session_id, const Snapshot& s, bool state, bool full_resync)
{
    json j = envelope(state ? "state" : "snapshot", session_id, s.generation);
    if (state) {
        j["full_resync"] = full_resync;
        j["spec"] = spec_json(s.spec);
        j["run_id"] = name(s.run_id);
    }
    j["symbol"] = name(s.spec.ticker);
    j["snapshot_seq"] = s.snapshot_seq;
    j["exchange_ts"] = s.exchange_ts;
    j["event_seq"] = s.event_seq;
    j["last_report_seq"] = s.last_report_seq;
    j["last_command_seq"] = s.last_command_seq;
    j["state"] = s.state;
    j["speed"] = s.speed;
    j["progress"] = s.progress;
    j["failure"] = s.failure;
    j["source_error"] = source_error(s);
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
        j["symbol"] = name(r.control.spec.ticker);
        j["strategy"] = r.control.spec.strategy.kind;
        j["days"] = days_json(r.control.spec.days);
        j["window"] = r.control.spec.window;
        j["run_id"] = name(r.control.watch.run_id);
        break;
    case RecordKind::GenerationEnd:
        j["kind"] = "generation_end";
        j["exchange_ts"] = r.exchange_ts;
        j["market_event_seq"] = r.market_event_seq;
        break;
    case RecordKind::Verdict:
        j["kind"] = "reproduced";
        j["run_id"] = name(r.control.watch.run_id);
        j["outcome"] = r.outcome;
        j["digests"] = digests_json(r.digests);
        j["expected"] = digests_json(r.control.watch.expected);
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

std::string encode_days(std::chrono::year_month_day start, std::span<const DayHandle> days)
{
    return text({{"start", iso_date(start)}, {"days", array_of(days, day_json)}});
}

std::string encode_history(std::string_view session_id, std::uint64_t generation, std::span<const Candle> candles,
                           std::span<const Trade> trades)
{
    return text({{"session_id", session_id},
                 {"generation", generation},
                 {"candles", array_of(candles, candle_json)},
                 {"trades", array_of(trades, trade_json)}});
}

bool decode_run_spec(std::string_view text, RunSpec& spec, std::string& field)
{
    field.clear();
    const json j = json::parse(text, nullptr, false);
    return j.is_object() && spec_from(j, spec, field);
}

std::string encode_strategies()
{
    const auto param = [](const char* name, const json& value, const json& min, const json& max) {
        return json{{"name", name}, {"default", value}, {"min", min}, {"max", max}};
    };
    const RunSpec mm = default_spec(SessionStrategyKind::MarketMaker);
    const RunSpec it = default_spec(SessionStrategyKind::ImbalanceTaker);
    const RunSpec orb = default_spec(SessionStrategyKind::Orb);
    const MarketMakerParams& m = mm.strategy.market_maker;
    const ImbalanceTakerParams& i = it.strategy.imbalance;
    const OrbParams& o = orb.strategy.orb;
    const json strategies =
        json::array({{{"kind", SessionStrategyKind::MarketMaker},
                      {"params",
                       json::array({param("quote_size", m.quote_size, 1, max_param),
                                    param("max_position", m.max_position, 0, max_param),
                                    param("requote_cooldown_ns", m.requote_cooldown_ns, 0, max_cooldown_ns),
                                    param("improve_ticks", m.improve_ticks, 0, max_param)})},
                      {"limits", limits_json(mm)}},
                     {{"kind", SessionStrategyKind::ImbalanceTaker},
                      {"params",
                       json::array({param("threshold", i.threshold, 1, nullptr), param("clip", i.clip, 1, max_param),
                                    param("max_position", i.max_position, 0, max_param),
                                    param("cooldown_ns", i.cooldown_ns, 0, max_cooldown_ns)})},
                      {"limits", limits_json(it)}},
                     {{"kind", SessionStrategyKind::Orb},
                      {"params",
                       json::array({param("range_minutes", o.range_minutes, 1, max_range_minutes),
                                    param("clip", o.clip, 1, max_param)})},
                      {"limits", limits_json(orb)}}});
    return text(
        {{"strategies", strategies},
         {"latency",
          {{"order_ns", mm.latency.order_latency}, {"report_ns", mm.latency.report_latency}, {"max", max_latency_ns}}},
         {"fees",
          {{"maker", mm.fees.maker_fee_per_share},
           {"taker", mm.fees.taker_fee_per_share},
           {"min", -max_fee},
           {"max", max_fee},
           {"taker_min", 0}}},
         {"limit_max", max_param},
         {"windows",
          json::array({{{"id", TradingWindow::Regular}, {"label", "Regular (feed session)"}},
                       {{"id", TradingWindow::FullDay}, {"label", "Full day"}}})},
         {"max_days", max_days},
         {"max_queued", max_queued}});
}

std::string encode_run(const Run& run)
{
    return text(run_json(run, true));
}

bool decode_run(std::string_view text, Run& run)
{
    const json j = json::parse(text, nullptr, false);
    Run r;
    std::string field;
    try {
        if (!j.is_object() || j.at("format") != 1 || !spec_from(j.at("spec"), r.spec, field)) {
            return false;
        }
        r.id = j.at("id").get<std::string>();
        r.state = name_from<RunState>(j.at("state"));
        r.reason = j.at("reason").get<std::string>();
        r.warning = j.at("warning").get<std::string>();
        const json& p = j.at("provenance");
        const json& build = p.at("build");
        const json& engine = p.at("engine");
        r.provenance = {.build = {.commit = build.at("commit").get<std::string>(),
                                  .dirty = build.at("dirty").get<bool>(),
                                  .source_digest = build.at("source_digest").get<std::string>(),
                                  .compiler = build.at("compiler").get<std::string>(),
                                  .build_type = build.at("build_type").get<std::string>(),
                                  .sanitize = build.at("sanitize").get<std::string>(),
                                  .platform = build.at("platform").get<std::string>()},
                        .book_capacity = engine.at("book_capacity").get<std::size_t>(),
                        .price_scale = engine.at("price_scale").get<std::uint8_t>(),
                        .quantity_scale = engine.at("quantity_scale").get<std::uint8_t>(),
                        .check_invariants_every = engine.at("check_invariants_every").get<std::uint64_t>(),
                        .allow_truncated_tail = p.at("allow_truncated_tail").get<bool>(),
                        .host = p.at("host").get<std::string>(),
                        .submitted = p.at("submitted").get<std::string>(),
                        .started = p.at("started").get<std::string>(),
                        .finished = p.at("finished").get<std::string>(),
                        .wall_ms = p.at("wall_ms").get<std::int64_t>(),
                        .cpu_ms = p.at("cpu_ms").get<std::int64_t>()};
        for (const json& day : p.at("days")) {
            RunDay& d = r.provenance.days.emplace_back(RunDay{.date = date_from(day.at("date")),
                                                              .file = day.at("file").get<std::string>(),
                                                              .size = day.at("size").get<std::uint64_t>(),
                                                              .hash = day.at("hash").get<std::string>(),
                                                              .session = day.at("session").get<FeedSession>()});
            if (const json segment = day.value("segment", json(nullptr));
                !segment.is_null()) {  // none before the store
                d.segment_hash = segment.at("hash").get<std::string>();
                d.segment_size = segment.at("size").get<std::uint64_t>();
            }
        }
        if (!j.at("results").is_null()) {
            r.results = std::make_shared<const RunResult>(results_from(j.at("results")));
        }
        for (const json& v : j.at("verifications")) {
            Verification& verification =
                r.verifications.emplace_back(Verification{.finished = v.at("finished").get<std::string>(),
                                                          .outcome = name_from<Outcome>(v.at("outcome")),
                                                          .source_digest = v.at("source_digest").get<std::string>()});
            if (!v.at("digests").is_null()) {
                verification.digests = digests_from(v.at("digests"));
            }
            for (const json& date : v.at("changed")) {
                verification.changed.push_back(date_from(date));
            }
        }
    } catch (const std::exception&) {  // nlohmann's, or a name, date or digest that does not parse
        return false;
    }
    if (r.state == RunState::Queued || r.state == RunState::Running) {
        return false;
    }
    run = std::move(r);
    return true;
}

std::string encode_lab_error(std::string_view code, std::string_view field, std::span<const std::string> days)
{
    json j = {{"error", code}};
    if (!field.empty()) {
        j["field"] = field;
    }
    if (!days.empty()) {
        j["days"] = days;
    }
    return text(j);
}

std::string encode_run_id(std::string_view id)
{
    return text({{"id", id}});
}

std::string encode_warning(std::string_view warning)
{
    return text({{"warning", warning}});
}

std::string encode_runs(std::span<const Run> runs, std::size_t queued)
{
    json list = json::array();
    for (const Run& run : runs) {
        json entry = run_json(run, false);
        entry["progress"] = run.progress;
        entry["partial"] = run.results != nullptr && run.results->partial;
        entry["pnl"] = run.results ? json(run.results->pnl) : json(nullptr);
        entry["fills"] = run.results ? json(run.results->maker_fills + run.results->taker_fills) : json(nullptr);
        list.push_back(std::move(entry));
    }
    return text({{"runs", list}, {"queued", queued}, {"max_queued", max_queued}});
}

}  // namespace simulator::server
