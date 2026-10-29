#include "chronos/server/script_strategy.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

// Lua is compiled as C++ (CMakeLists.txt), so its headers are included as they are: a script's error then unwinds
// through C++ frames as an exception, and its functions have C++ linkage.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace simulator::server {
namespace {

constexpr int hook_every = 1'000;  // instructions between two calls of the budget's hook
constexpr std::uint64_t hooks_per_call = script_instruction_budget / hook_every;
// Work a call does inside Lua's C library, which the instruction count does not see, is charged as the bytes it
// allocates. Comparing or searching long strings allocates nothing: the wall-clock limit is the net for that.
constexpr std::size_t allocation_budget = std::size_t{1} << 30;
// Generous: 10 million instructions take a few seconds even in a sanitized debug build.
constexpr auto call_time_limit = std::chrono::seconds{30};
constexpr const char* time_limit_message = "over 30 s in one call";
constexpr std::size_t max_sends_per_call = 256;  // orders and cancels: each publishes a record in a session
constexpr std::size_t max_pending_timers = 64;
constexpr std::size_t max_params = 32;
constexpr std::size_t max_find_pattern = 256;
constexpr lua_Integer max_rep = lua_Integer{1} << 24;
constexpr std::size_t max_error = 300;
constexpr lua_Integer max_timer_id = lua_Integer{1} << 62;  // clear of the session's close timer
constexpr lua_Integer max_size = 1'000'000'000;  // a price or a quantity, as the shipped strategies cap them
constexpr std::size_t max_log_line = 300;

// `text` cut to at most `max` bytes, never inside a UTF-8 sequence.
std::string cut(std::string text, std::size_t max)
{
    if (text.size() > max) {
        std::size_t n = max;
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) {
            --n;
        }
        text.resize(n);
    }
    return text;
}
constexpr std::size_t max_stat_name = 40;
constexpr std::size_t max_help = 200;
constexpr std::size_t max_depth = 64;
constexpr lua_Integer random_seed = 20'191'230;  // every run's math.random starts here

// A script's limits until it declares its own.
RiskLimits default_script_limits()
{
    return {.max_position = 1'000, .max_order_quantity = Quantity{1'000}, .max_open_orders = 10};
}

const char* event_name(MarketEventType type)
{
    switch (type) {
    case MarketEventType::Add:
        return "add";
    case MarketEventType::Modify:
        return "modify";
    case MarketEventType::Replace:
        return "replace";
    case MarketEventType::Cancel:
        return "cancel";
    case MarketEventType::Delete:
        return "delete";
    case MarketEventType::Execute:
        return "execute";
    case MarketEventType::Clear:
        return "clear";
    case MarketEventType::Trade:
        return "trade";
    case MarketEventType::Status:
        return "status";
    }
    return "unknown";
}

const char* side_name(Side side)
{
    return side == Side::Buy ? "buy" : "sell";
}

const char* report_name(ReportType type)
{
    switch (type) {
    case ReportType::Accepted:
        return "accepted";
    case ReportType::Rejected:
        return "rejected";
    case ReportType::Fill:
        return "fill";
    case ReportType::Cancelled:
        return "cancelled";
    case ReportType::CancelRejected:
        return "cancel_rejected";
    case ReportType::Expired:
        return "expired";
    }
    return "unknown";
}

const char* reason_name(RejectReason reason)
{
    switch (reason) {
    case RejectReason::None:
        return "none";
    case RejectReason::WouldCross:
        return "would_cross";
    case RejectReason::InvalidOrder:
        return "invalid_order";
    case RejectReason::NotTrading:
        return "not_trading";
    case RejectReason::OffTick:
        return "off_tick";
    }
    return "unknown";
}

const char* liquidity_name(Liquidity liquidity)
{
    switch (liquidity) {
    case Liquidity::None:
        return "none";
    case Liquidity::Maker:
        return "maker";
    case Liquidity::Taker:
        return "taker";
    }
    return "unknown";
}

const char* risk_name(RiskCheck check)
{
    switch (check) {
    case RiskCheck::Accepted:
        return "accepted";
    case RiskCheck::InvalidOrder:
        return "invalid_order";
    case RiskCheck::OrderTooLarge:
        return "order_too_large";
    case RiskCheck::PositionLimit:
        return "position_limit";
    case RiskCheck::TooManyOpenOrders:
        return "too_many_open_orders";
    case RiskCheck::Closed:
        return "closed";
    }
    return "unknown";
}

// One Lua state running one script. Every call into Lua is a protected call, so a script's error, its memory running
// out or its instruction budget running out stops the strategy with the reason, and never escapes into the engine.
class ScriptStrategy final : public AnyStrategy
{
public:
    lua_State* L{nullptr};
    std::size_t memory{0};
    std::uint64_t hooks{0};       // the budget's hook calls in the current call
    std::size_t allocated{0};     // bytes allocated in the current call
    bool spent{false};            // the current call went over a budget: every instruction raises until it returns
    const char* spent_on{nullptr};
    std::chrono::steady_clock::time_point deadline{};
    std::size_t sends{0};     // orders and cancels in the current call
    std::size_t pending{0};   // timers set and not yet fired
    bool top_level{false};  // param() and limits() only while the top level runs
    StrategyContext* ctx{nullptr};
    const MarketEvent* event{nullptr};
    const ExecutionReport* report{nullptr};
    std::uint64_t timer_id{0};
    std::string_view source;  // while loading
    const ParamList* run_params{nullptr};  // while loading for a run: the values param() returns
    std::vector<ParamInfo> declared;
    RiskLimits limits{default_script_limits()};
    std::string summary;
    std::string failure;
    std::vector<Stat> counters;
    std::vector<std::string> lines;
    std::size_t dropped_lines{0};
    std::vector<DaySession> sessions;
    unsigned price_scale{4};
    int on_market_ref{LUA_NOREF};
    int on_report_ref{LUA_NOREF};
    int on_timer_ref{LUA_NOREF};
    int event_ref{LUA_NOREF};
    int report_ref{LUA_NOREF};

    ScriptStrategy(std::string_view text, const ParamList* params, const StrategySetup& setup);
    ~ScriptStrategy() override
    {
        if (L != nullptr) {
            lua_close(L);
        }
    }

    void on_market(StrategyContext& c, const MarketEvent& e) noexcept override
    {
        if (on_market_ref != LUA_NOREF) {
            ctx = &c;
            event = &e;
            protect(market_call, "on_market");
            ctx = nullptr;
        }
    }
    void on_report(StrategyContext& c, const ExecutionReport& r) noexcept override
    {
        if (on_report_ref != LUA_NOREF) {
            ctx = &c;
            report = &r;
            protect(report_call, "on_report");
            ctx = nullptr;
        }
    }
    void on_timer(StrategyContext& c, std::uint64_t id) noexcept override
    {
        pending -= pending > 0 ? 1 : 0;
        if (on_timer_ref != LUA_NOREF) {
            ctx = &c;
            timer_id = id;
            protect(timer_call, "on_timer");
            ctx = nullptr;
        }
    }
    void stats(std::vector<Stat>& out) const override
    {
        out.insert(out.end(), counters.begin(), counters.end());
    }
    std::string_view error() const noexcept override
    {
        return failure;
    }
    std::span<const std::string> log() const noexcept override
    {
        return lines;
    }

    // Runs `f` in protected mode; on an error, records it as where the script stopped and stops it.
    bool protect(lua_CFunction f, const char* where) noexcept
    {
        if (L == nullptr || !failure.empty()) {
            return false;
        }
        hooks = 0;
        allocated = 0;
        spent = false;
        sends = 0;
        deadline = std::chrono::steady_clock::now() + call_time_limit;
        lua_sethook(L, budget_hook, LUA_MASKCOUNT, hook_every);  // as before a call that went over budget
        lua_pushcfunction(L, f);  // a light C function: pushing it allocates nothing
        if (lua_pcall(L, 0, 0, 0) == LUA_OK) {
            return true;
        }
        const char* message = lua_tostring(L, -1);
        failure =
            std::string{where} + ": " + cut(message != nullptr ? message : "an error that is not a string", max_error);
        lua_pop(L, 1);
        return false;
    }

    void append_line(std::string line)
    {
        if (lines.size() < max_script_log_lines) {
            lines.push_back(cut(std::move(line), max_log_line));
        } else {
            ++dropped_lines;
        }
    }

    static void budget_hook(lua_State* L, lua_Debug* ar);
    static int load_call(lua_State* L);
    static int market_call(lua_State* L);
    static int report_call(lua_State* L);
    static int timer_call(lua_State* L);
};

ScriptStrategy& self(lua_State* L)
{
    return **static_cast<ScriptStrategy**>(lua_getextraspace(L));
}

// The state's memory, refused past script_memory_cap: Lua then raises "not enough memory".
void* allocate(void* ud, void* ptr, std::size_t old_size, std::size_t new_size)
{
    ScriptStrategy& s = *static_cast<ScriptStrategy*>(ud);
    const std::size_t old = ptr != nullptr ? old_size : 0;
    if (new_size == 0) {
        std::free(ptr);
        s.memory -= old;
        return nullptr;
    }
    if (new_size > old && s.memory - old + new_size > script_memory_cap) {
        return nullptr;
    }
    void* moved = std::realloc(ptr, new_size);
    if (moved != nullptr) {
        s.memory = s.memory - old + new_size;
        s.allocated += new_size > old ? new_size - old : 0;
    }
    return moved;
}

}  // namespace

// Over a budget, the call is spent: from then on the hook fires and raises at every instruction, so a pcall in the
// script cannot catch the error and carry on. In a hook, level 0 is the script's running function, which luaL_error
// would skip. The count and the bytes are deterministic; the wall clock is the net for what neither sees.
void ScriptStrategy::budget_hook(lua_State* L, lua_Debug*)
{
    ScriptStrategy& s = self(L);
    if (!s.spent) {
        if (++s.hooks > hooks_per_call) {
            s.spent_on = "over 10 million instructions in one call: an endless loop?";
        } else if (s.allocated > allocation_budget) {
            s.spent_on = "over 1 GiB allocated in one call";
        } else if (std::chrono::steady_clock::now() > s.deadline) {
            s.spent_on = time_limit_message;
        } else {
            return;
        }
        s.spent = true;
        lua_sethook(L, budget_hook, LUA_MASKCOUNT, 1);
    }
    luaL_where(L, 0);
    lua_pushstring(L, s.spent_on);
    lua_concat(L, 2);
    lua_error(L);
}

namespace {

StrategyContext& context(lua_State* L, const char* function)
{
    ScriptStrategy& s = self(L);
    if (s.ctx == nullptr) {
        luaL_error(L, "%s() works only inside on_market, on_report or on_timer", function);
    }
    return *s.ctx;
}

void set_integer(lua_State* L, const char* key, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}

void set_string(lua_State* L, const char* key, const char* value)
{
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}

// A number argument as a ParamValue of the param's kind; false if it is not one.
bool param_value(lua_State* L, int index, bool integer, ParamValue& value)
{
    if (lua_type(L, index) != LUA_TNUMBER) {
        return false;
    }
    if (integer) {
        int is_integer = 0;
        const lua_Integer i = lua_tointegerx(L, index, &is_integer);  // a float with an exact integer value too
        value = std::int64_t{i};
        return is_integer != 0;
    }
    value = static_cast<double>(lua_tonumber(L, index));
    return true;
}

// param(name, default, {min = , max = , help = }): declares a param and returns its value in this run.
int l_param(lua_State* L)
{
    ScriptStrategy& s = self(L);
    if (!s.top_level) {
        return luaL_error(L, "param() works only at the top level of the script");
    }
    const char* name = luaL_checkstring(L, 1);
    if (!valid_strategy_name(name)) {
        return luaL_error(L, "param '%s': a name is 1-40 of a-z, 0-9 and _", name);
    }
    if (s.declared.size() >= max_params) {
        return luaL_error(L, "param: at most %d params", static_cast<int>(max_params));
    }
    for (const ParamInfo& p : s.declared) {
        if (p.name == name) {
            return luaL_error(L, "param '%s' is declared twice", name);
        }
    }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        return luaL_error(L, "param '%s': the default must be a number", name);
    }
    const bool integer = lua_isinteger(L, 2) != 0;
    ParamInfo info{.name = name};
    param_value(L, 2, integer, info.default_value);
    info.min = integer ? ParamValue{std::int64_t{0}} : ParamValue{0.0};
    info.max = integer ? ParamValue{std::int64_t{max_size}} : ParamValue{std::numeric_limits<double>::infinity()};
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        if (lua_getfield(L, 3, "min") != LUA_TNIL && !param_value(L, -1, integer, info.min)) {
            return luaL_error(L, "param '%s': min must be %s", name, integer ? "an integer" : "a number");
        }
        if (lua_getfield(L, 3, "max") != LUA_TNIL && !param_value(L, -1, integer, info.max)) {
            return luaL_error(L, "param '%s': max must be %s", name, integer ? "an integer" : "a number");
        }
        if (lua_getfield(L, 3, "help") != LUA_TNIL) {
            std::size_t length = 0;
            const char* help = lua_tolstring(L, -1, &length);
            if (help == nullptr || length > max_help) {
                return luaL_error(L, "param '%s': help must be a string of at most 200 characters", name);
            }
            info.help.assign(help, length);
        }
        lua_pop(L, 3);
    }
    const auto as_double = [](const ParamValue& v) {
        return std::holds_alternative<double>(v) ? std::get<double>(v) : static_cast<double>(std::get<std::int64_t>(v));
    };
    if (!std::isfinite(as_double(info.min)) || std::isnan(as_double(info.max)) ||
        as_double(info.min) > as_double(info.max) || !param_fits(info, info.default_value)) {
        return luaL_error(L, "param '%s': needs min <= default <= max, min finite", name);
    }
    const ParamValue* value = s.run_params != nullptr ? find_param(*s.run_params, name) : nullptr;
    const ParamValue& v = value != nullptr ? *value : info.default_value;
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        lua_pushinteger(L, *i);
    } else {
        lua_pushnumber(L, std::get<double>(v));
    }
    s.declared.push_back(std::move(info));
    return 1;
}

// limits{max_position = , max_order_quantity = , max_open_orders = }: the run's risk limits unless it sets its own.
int l_limits(lua_State* L)
{
    ScriptStrategy& s = self(L);
    if (!s.top_level) {
        return luaL_error(L, "limits() works only at the top level of the script");
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto read = [&](const char* key, lua_Integer min, lua_Integer& value) {
        if (lua_getfield(L, 1, key) != LUA_TNIL) {
            int is_integer = 0;
            value = lua_tointegerx(L, -1, &is_integer);
            if (is_integer == 0 || value < min || value > max_size) {
                luaL_error(L, "limits: %s must be an integer from %d to 1000000000", key, static_cast<int>(min));
            }
        }
        lua_pop(L, 1);
    };
    lua_Integer position = s.limits.max_position;
    lua_Integer order = static_cast<lua_Integer>(s.limits.max_order_quantity.value());
    lua_Integer open = static_cast<lua_Integer>(s.limits.max_open_orders);
    read("max_position", 0, position);
    read("max_order_quantity", 1, order);
    read("max_open_orders", 1, open);
    s.limits = {.max_position = position,
                .max_order_quantity = Quantity{static_cast<std::uint64_t>(order)},
                .max_open_orders = static_cast<std::size_t>(open)};
    return 0;
}

int l_now(lua_State* L)
{
    lua_pushinteger(L, context(L, "now").now());
    return 1;
}

int l_trading(lua_State* L)
{
    lua_pushboolean(L, context(L, "trading").book.trading_state() == TradingState::Trading);
    return 1;
}

int push_level(lua_State* L, bool found, const LevelInfo& level)
{
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, level.price.ticks);
    lua_pushinteger(L, static_cast<lua_Integer>(level.quantity));
    lua_pushinteger(L, level.order_count);
    return 3;
}

int l_best_bid(lua_State* L)
{
    LevelInfo level;
    const bool found = context(L, "best_bid").book.best(Side::Buy, level);
    return push_level(L, found, level);
}

int l_best_ask(lua_State* L)
{
    LevelInfo level;
    const bool found = context(L, "best_ask").book.best(Side::Sell, level);
    return push_level(L, found, level);
}

Side side_arg(lua_State* L, int index)
{
    const std::string_view side = luaL_checkstring(L, index);
    if (side != "buy" && side != "sell") {
        luaL_argerror(L, index, "'buy' or 'sell'");
    }
    return side == "buy" ? Side::Buy : Side::Sell;
}

// level(side, n): the price, shares and orders of the nth best level (1 is the best) of a side, or nil.
int l_level(lua_State* L)
{
    const MarketBook& book = context(L, "level").book;
    const Side side = side_arg(L, 1);
    const lua_Integer n = luaL_checkinteger(L, 2);
    luaL_argcheck(L, n >= 1 && n <= static_cast<lua_Integer>(max_depth), 2, "from 1 to 64");
    std::array<LevelInfo, max_depth> levels{};
    const std::size_t got = book.depth(side, std::span{levels}.first(static_cast<std::size_t>(n)));
    return push_level(L, got == static_cast<std::size_t>(n), levels[static_cast<std::size_t>(n) - 1]);
}

int l_position(lua_State* L)
{
    lua_pushinteger(L, context(L, "position").portfolio.position());
    return 1;
}

int l_cash(lua_State* L)
{
    lua_pushinteger(L, context(L, "cash").portfolio.cash());
    return 1;
}

int l_pnl(lua_State* L)
{
    lua_pushinteger(L, context(L, "pnl").portfolio.pnl());
    return 1;
}

int l_open_orders(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(context(L, "open_orders").portfolio.open_orders()));
    return 1;
}

// buy/sell(price, qty, tif): tif "ioc" (the default) or "post_only"; the order's id, or nil and why it was refused.
// One more order or cancel in this call, or an error past the cap: each publishes a record, and a session's log must keep
// up with them.
void count_send(lua_State* L)
{
    if (++self(L).sends > max_sends_per_call) {
        luaL_error(L, "over %d orders and cancels in one call", static_cast<int>(max_sends_per_call));
    }
}

int send(lua_State* L, Side side)
{
    StrategyContext& ctx = context(L, side == Side::Buy ? "buy" : "sell");
    count_send(L);
    const lua_Integer price = luaL_checkinteger(L, 1);
    const lua_Integer quantity = luaL_checkinteger(L, 2);
    const std::string_view tif = luaL_optstring(L, 3, "ioc");
    luaL_argcheck(L, price > 0 && price <= max_size * 10'000, 1, "a positive price in ticks");
    luaL_argcheck(L, quantity > 0 && quantity <= max_size, 2, "from 1 to 1000000000 shares");
    luaL_argcheck(L, tif == "ioc" || tif == "post_only", 3, "'ioc' or 'post_only'");
    OrderId id{0};
    const RiskCheck check = ctx.gateway.send(side, Price{price}, Quantity{static_cast<std::uint64_t>(quantity)},
                                             tif == "ioc" ? TimeInForce::Ioc : TimeInForce::Day, id);
    if (check == RiskCheck::Accepted) {
        lua_pushinteger(L, static_cast<lua_Integer>(id));
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, risk_name(check));
    return 2;
}

int l_buy(lua_State* L)
{
    return send(L, Side::Buy);
}

int l_sell(lua_State* L)
{
    return send(L, Side::Sell);
}

int l_cancel(lua_State* L)
{
    StrategyContext& ctx = context(L, "cancel");
    const lua_Integer id = luaL_checkinteger(L, 1);
    luaL_argcheck(L, id > 0, 1, "an order id");
    count_send(L);
    ctx.gateway.cancel(static_cast<OrderId>(id));
    return 0;
}

// set_timer(at, id): on_timer(id) at exchange time `at`, after now(); false if refused, or if 64 are already pending.
// A timer at now() could fire forever without time moving on.
int l_set_timer(lua_State* L)
{
    ScriptStrategy& s = self(L);
    StrategyContext& ctx = context(L, "set_timer");
    const lua_Integer at = luaL_checkinteger(L, 1);
    const lua_Integer id = luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, id >= 0 && id < max_timer_id, 2, "from 0 to 2^62");
    const bool set =
        at > ctx.now() && s.pending < max_pending_timers && ctx.set_timer(at, static_cast<std::uint64_t>(id));
    s.pending += set ? 1 : 0;
    lua_pushboolean(L, set);
    return 1;
}

// session(): the open and close of the regular session that now() is in or before, or nil after the last.
int l_session(lua_State* L)
{
    ScriptStrategy& s = self(L);
    const Timestamp now = context(L, "session").now();
    for (const DaySession& day : s.sessions) {
        if (now < day.close) {
            lua_pushinteger(L, day.open);
            lua_pushinteger(L, day.close);
            return 2;
        }
    }
    lua_pushnil(L);
    return 1;
}

// Whether the value at `index` has a type that tostring would show by its address.
bool shown_by_address(lua_State* L, int index)
{
    const int type = lua_type(L, index);
    if (type != LUA_TTABLE && type != LUA_TFUNCTION && type != LUA_TTHREAD && type != LUA_TUSERDATA &&
        type != LUA_TLIGHTUSERDATA) {
        return false;
    }
    if (luaL_getmetafield(L, index, "__tostring") != LUA_TNIL) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Pushes the value at `index` as text: an address, which changes from run to run, shows as its type alone.
void push_text(lua_State* L, int index)
{
    if (shown_by_address(L, index)) {
        lua_pushstring(L, luaL_typename(L, index));
    } else {
        luaL_tolstring(L, index, nullptr);
    }
}

int l_tostring(lua_State* L)
{
    luaL_checkany(L, 1);
    push_text(L, 1);
    return 1;
}

// string.format, without %p, and with values tostring would show by address shown by type.
int l_format(lua_State* L)
{
    std::size_t length = 0;
    const std::string_view format{luaL_checklstring(L, 1, &length), length};
    for (std::size_t i = format.find('%'); i != std::string_view::npos; i = format.find('%', i)) {
        i = format.find_first_not_of("-+ #0123456789.", i + 1);
        if (i == std::string_view::npos) {
            break;
        }
        if (format[i] == 'p') {
            return luaL_error(L, "string.format: %%p shows an address, which changes from run to run");
        }
        ++i;  // past the conversion, a second % included
    }
    for (int i = 2; i <= lua_gettop(L); ++i) {
        if (shown_by_address(L, i)) {
            lua_pushstring(L, luaL_typename(L, i));
            lua_replace(L, i);
        }
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// string.find on plain text only, its needle at most 256 bytes: Lua's patterns can backtrack for ever inside one C
// call, out of the budget's sight.
int l_find(lua_State* L)
{
    luaL_checkstring(L, 1);
    std::size_t length = 0;
    luaL_checklstring(L, 2, &length);
    luaL_argcheck(L, length <= max_find_pattern, 2, "at most 256 bytes");
    const lua_Integer init = luaL_optinteger(L, 3, 1);
    lua_settop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_pushinteger(L, init);
    lua_pushboolean(L, 1);  // plain
    lua_call(L, 4, LUA_MULTRET);
    return lua_gettop(L);
}

// string.rep, at most 2^24 copies: copies of an empty string allocate nothing, so the budget would not see them.
int l_rep(lua_State* L)
{
    luaL_argcheck(L, luaL_checkinteger(L, 2) <= max_rep, 2, "at most 16777216 copies");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// setmetatable, refusing __gc: Lua runs a finalizer with hooks off, out of the budget's reach.
int l_setmetatable(lua_State* L)
{
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        const bool finalizer = lua_rawget(L, 2) != LUA_TNIL;
        lua_pop(L, 1);
        if (finalizer) {
            return luaL_error(L, "setmetatable: a metatable with __gc is not allowed");
        }
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// log(...): a line in the run's log, its values separated by spaces.
int l_log(lua_State* L)
{
    ScriptStrategy& s = self(L);
    const int n = lua_gettop(L);  // before luaL_buffinit, which pushes a placeholder
    luaL_Buffer line;
    luaL_buffinit(L, &line);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            luaL_addchar(&line, ' ');
        }
        push_text(L, i);
        luaL_addvalue(&line);
    }
    luaL_pushresult(&line);
    std::size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    s.append_line(std::string{text, length});
    return 0;
}

// stat(name, value [, is_price]): a counter shown after the run; a price in ticks is shown in dollars.
int l_stat(lua_State* L)
{
    ScriptStrategy& s = self(L);
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    const double value = static_cast<double>(luaL_checknumber(L, 2));
    const bool price = lua_toboolean(L, 3) != 0;
    luaL_argcheck(L, length > 0 && length <= max_stat_name, 1, "1 to 40 characters");
    luaL_argcheck(L, std::isfinite(value), 2, "a finite number");
    for (Stat& stat : s.counters) {
        if (stat.name == name) {
            stat.value = value;
            stat.price = price;
            return 0;
        }
    }
    if (s.counters.size() >= max_script_stats) {
        return luaL_error(L, "stat: at most %d stats", static_cast<int>(max_script_stats));
    }
    s.counters.push_back({.name = std::string{name, length}, .value = value, .price = price});
    return 0;
}

// dollars(ticks) and ticks(dollars): a price at this instrument's scale, ticks rounded to the nearest.
int l_dollars(lua_State* L)
{
    lua_pushnumber(L, luaL_checknumber(L, 1) / std::pow(10.0, self(L).price_scale));
    return 1;
}

int l_ticks(lua_State* L)
{
    const double ticks = std::round(luaL_checknumber(L, 1) * std::pow(10.0, self(L).price_scale));
    luaL_argcheck(L, std::isfinite(ticks) && std::abs(ticks) < 9e15, 1, "a price in dollars");
    lua_pushinteger(L, static_cast<lua_Integer>(ticks));
    return 1;
}

// math.randomseed(n [, m]): a seed of the script's own; without one Lua would seed from the clock.
int l_randomseed(lua_State* L)
{
    luaL_checkinteger(L, 1);
    luaL_optinteger(L, 2, 0);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 0);
    return 0;
}

// table.sort's comparisons, and table.insert's and table.remove's shifts, as Lua code the budget counts; table.move,
// whose loop could run for 2^63 indices, is gone.
constexpr const char* table_prelude = R"lua(
local sort, insert, remove, mtype = table.sort, table.insert, table.remove, math.type
local function less(a, b) return a < b end
table.sort = function(t, f) return sort(t, f or less) end
table.insert = function(t, ...)
  local n = select("#", ...)
  if n == 1 then return insert(t, ...) end
  if n ~= 2 then error("wrong number of arguments to 'insert'", 2) end
  local pos, v = ...
  local e = #t + 1
  if mtype(pos) ~= "integer" or pos < 1 or pos > e then
    error("bad argument #2 to 'insert' (position out of bounds)", 2)
  end
  for i = e, pos + 1, -1 do t[i] = t[i - 1] end
  t[pos] = v
end
table.remove = function(t, pos)
  local size = #t
  if pos == nil or pos == size then return remove(t, pos) end
  if mtype(pos) ~= "integer" or pos < 1 or pos > size + 1 then
    error("bad argument #2 to 'remove' (position out of bounds)", 2)
  end
  local v = t[pos]
  for i = pos, size - 1 do t[i] = t[i + 1] end
  t[pos < size and size or pos] = nil
  return v
end
table.move = nil
)lua";

const luaL_Reg api[] = {
    {"param", l_param},
    {"limits", l_limits},
    {"now", l_now},
    {"trading", l_trading},
    {"best_bid", l_best_bid},
    {"best_ask", l_best_ask},
    {"level", l_level},
    {"position", l_position},
    {"cash", l_cash},
    {"pnl", l_pnl},
    {"open_orders", l_open_orders},
    {"buy", l_buy},
    {"sell", l_sell},
    {"cancel", l_cancel},
    {"set_timer", l_set_timer},
    {"session", l_session},
    {"log", l_log},
    {"print", l_log},
    {"stat", l_stat},
    {"dollars", l_dollars},
    {"ticks", l_ticks},
    {nullptr, nullptr},
};

// The callback a script defines as the global `name`, as a registry reference; LUA_NOREF if it defines none.
int callback(lua_State* L, const char* name)
{
    const int type = lua_getglobal(L, name);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    if (type != LUA_TFUNCTION) {
        luaL_error(L, "%s is a %s, not a function", name, lua_typename(L, type));
    }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

}  // namespace

// The sandbox: the base library without its file, load and collector functions; string, table, math and utf8; then
// the API, the reused event and report tables, the script's top level, and its callbacks.
int ScriptStrategy::load_call(lua_State* L)
{
    ScriptStrategy& s = self(L);
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 5);
    // xpcall: its handler runs with hooks off. collectgarbage("count") would show memory, which a run need not repeat.
    for (const char* unsafe : {"dofile", "loadfile", "load", "collectgarbage", "require", "xpcall"}) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    const auto wrap = [L](const char* table, const char* name, lua_CFunction f) {
        if (table != nullptr) {
            lua_getglobal(L, table);
            lua_getfield(L, -1, name);
            lua_pushcclosure(L, f, 1);
            lua_setfield(L, -2, name);
            lua_pop(L, 1);
        } else {
            lua_getglobal(L, name);
            lua_pushcclosure(L, f, 1);
            lua_setglobal(L, name);
        }
    };
    wrap(nullptr, "setmetatable", l_setmetatable);
    wrap(LUA_STRLIBNAME, "format", l_format);
    wrap(LUA_STRLIBNAME, "find", l_find);
    wrap(LUA_STRLIBNAME, "rep", l_rep);
    lua_pushcfunction(L, l_tostring);
    lua_setglobal(L, "tostring");
    lua_getglobal(L, LUA_STRLIBNAME);
    for (const char* patterns : {"match", "gmatch", "gsub"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, patterns);
    }
    lua_pop(L, 1);
    // Table functions whose C loops the budget would not see, in Lua, where it does.
    if (luaL_loadbufferx(L, table_prelude, std::strlen(table_prelude), "=table", "t") != LUA_OK) {
        return lua_error(L);
    }
    lua_call(L, 0, 0);
    // math.random from a fixed seed, and math.randomseed only with one.
    lua_getglobal(L, LUA_MATHLIBNAME);
    lua_getfield(L, -1, "randomseed");
    lua_pushvalue(L, -1);
    lua_pushinteger(L, random_seed);
    lua_call(L, 1, 0);
    lua_pushcclosure(L, l_randomseed, 1);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    lua_pushglobaltable(L);
    luaL_setfuncs(L, api, 0);
    lua_pop(L, 1);
    lua_pushinteger(L, s.price_scale);
    lua_setglobal(L, "price_scale");
    lua_newtable(L);
    s.event_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    s.report_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // "t": text only, so no hand-made bytecode reaches the VM.
    if (luaL_loadbufferx(L, s.source.data(), s.source.size(), "=script", "t") != LUA_OK) {
        return lua_error(L);
    }
    s.top_level = true;
    lua_call(L, 0, 0);
    s.top_level = false;
    s.on_market_ref = callback(L, "on_market");
    s.on_report_ref = callback(L, "on_report");
    s.on_timer_ref = callback(L, "on_timer");
    if (s.on_market_ref == LUA_NOREF && s.on_report_ref == LUA_NOREF && s.on_timer_ref == LUA_NOREF) {
        return luaL_error(L, "the script defines none of on_market, on_report and on_timer");
    }
    if (lua_getglobal(L, "summary") == LUA_TSTRING) {
        s.summary = lua_tostring(L, -1);
        if (s.summary.size() > max_help) {
            s.summary.resize(max_help);
        }
    }
    lua_pop(L, 1);
    return 0;
}

int ScriptStrategy::market_call(lua_State* L)
{
    ScriptStrategy& s = self(L);
    const MarketEvent& e = *s.event;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.on_market_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.event_ref);
    set_string(L, "type", event_name(e.type));
    set_string(L, "side", side_name(e.side));
    set_integer(L, "price", e.price.ticks);
    set_integer(L, "qty", static_cast<lua_Integer>(e.quantity.value()));
    set_integer(L, "time", e.timestamp);
    set_integer(L, "order_id", static_cast<lua_Integer>(e.order_id));
    if (const Trade* trade = s.ctx->trade) {
        set_integer(L, "trade_price", trade->price.ticks);
        set_integer(L, "trade_qty", static_cast<lua_Integer>(trade->quantity));
    } else {
        lua_pushnil(L);
        lua_setfield(L, -2, "trade_price");
        lua_pushnil(L);
        lua_setfield(L, -2, "trade_qty");
    }
    lua_call(L, 1, 0);
    return 0;
}

int ScriptStrategy::report_call(lua_State* L)
{
    ScriptStrategy& s = self(L);
    const ExecutionReport& r = *s.report;
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.on_report_ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.report_ref);
    set_string(L, "type", report_name(r.type));
    set_integer(L, "id", static_cast<lua_Integer>(r.order_id));
    set_string(L, "side", side_name(r.side));
    set_integer(L, "price", r.price.ticks);
    set_integer(L, "qty", static_cast<lua_Integer>(r.quantity.value()));
    set_integer(L, "leaves", static_cast<lua_Integer>(r.leaves.value()));
    set_string(L, "liquidity", liquidity_name(r.liquidity));
    set_string(L, "reason", reason_name(r.reason));
    set_integer(L, "time", r.exchange_time);
    lua_call(L, 1, 0);
    return 0;
}

int ScriptStrategy::timer_call(lua_State* L)
{
    ScriptStrategy& s = self(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, s.on_timer_ref);
    lua_pushinteger(L, static_cast<lua_Integer>(s.timer_id));
    lua_call(L, 1, 0);
    return 0;
}

ScriptStrategy::ScriptStrategy(std::string_view text, const ParamList* params, const StrategySetup& setup)
    : source(text),
      run_params(params),
      sessions(setup.sessions.begin(), setup.sessions.end()),
      price_scale(setup.price_scale)
{
    if (source.size() > max_script_bytes) {
        failure = "load: the script is over 48 KiB";
        return;
    }
    L = lua_newstate(allocate, this);
    if (L == nullptr) {
        failure = "load: not enough memory";
        return;
    }
    *static_cast<ScriptStrategy**>(lua_getextraspace(L)) = this;
    lua_sethook(L, budget_hook, LUA_MASKCOUNT, hook_every);
    protect(load_call, "load");
    source = {};
    run_params = nullptr;
}

// The listings describe every saved script on each request: a source's description at its defaults is kept, but for one
// the wall clock stopped, which need not repeat. ponytail: cleared when full, an LRU if many scripts churn.
ScriptInfo describe_script(std::string_view source, const ParamList* params)
{
    static std::mutex mutex;
    static std::map<std::string, ScriptInfo, std::less<>> described;
    if (params == nullptr) {
        const std::lock_guard lock{mutex};
        if (const auto it = described.find(source); it != described.end()) {
            return it->second;
        }
    }
    const ScriptStrategy script{source, params, StrategySetup{}};
    ScriptInfo info{
        .error = script.failure, .summary = script.summary, .params = script.declared, .limits = script.limits};
    if (params == nullptr && info.error.find(time_limit_message) == std::string::npos) {
        const std::lock_guard lock{mutex};
        if (described.size() >= 64) {
            described.clear();
        }
        described.emplace(std::string{source}, info);
    }
    return info;
}

std::unique_ptr<AnyStrategy> make_script_strategy(std::string_view source, const ParamList& params,
                                                  const StrategySetup& setup)
{
    return std::make_unique<ScriptStrategy>(source, &params, setup);
}

bool is_script_name(std::string_view name) noexcept
{
    return name.starts_with(script_prefix) && valid_strategy_name(name.substr(script_prefix.size()));
}

ScriptStore::ScriptStore(std::filesystem::path dir)
    : dir_(std::move(dir))
{}

std::vector<std::string> ScriptStore::names() const
{
    const std::lock_guard lock{mutex_};
    std::vector<std::string> names;
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator{dir_, ignored}) {
        const std::string stem = entry.path().stem().string();
        if (entry.path().extension() == ".lua" && valid_strategy_name(stem)) {
            names.push_back(stem);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ScriptStore::read(std::string_view name, std::string& source) const
{
    const std::lock_guard lock{mutex_};
    if (!valid_strategy_name(name)) {
        return false;
    }
    std::ifstream in{dir_ / (std::string{name} + ".lua"), std::ios::binary};
    if (!in) {
        return false;
    }
    source.assign(std::istreambuf_iterator<char>{in}, {});
    return true;
}

bool ScriptStore::write(std::string_view name, std::string_view source, std::string& error)
{
    const std::lock_guard lock{mutex_};
    if (!valid_strategy_name(name)) {
        error = "bad_name";
        return false;
    }
    const std::filesystem::path path = dir_ / (std::string{name} + ".lua");
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (!std::filesystem::exists(path, ec)) {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator{dir_, ec}) {
            count += entry.path().extension() == ".lua" ? 1 : 0;
        }
        if (count >= max_scripts) {
            error = "too_many";
            return false;
        }
    }
    const std::filesystem::path temp = dir_ / (std::string{name} + ".lua.tmp");
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        out.write(source.data(), static_cast<std::streamsize>(source.size()));
        if (!out.flush()) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        error = "cannot write " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool ScriptStore::remove(std::string_view name)
{
    const std::lock_guard lock{mutex_};
    std::error_code ec;
    return valid_strategy_name(name) && std::filesystem::remove(dir_ / (std::string{name} + ".lua"), ec);
}

}  // namespace simulator::server
