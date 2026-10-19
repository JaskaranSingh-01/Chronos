#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "chronos/server/candles.hpp"
#include "chronos/server/itch_input.hpp"
#include "chronos/server/replay_session.hpp"

// The WebSocket protocol (spec §6.3) and the HTTP bodies, as JSON text. Prices are integer ticks, quantities shares,
// times epoch nanoseconds (JavaScript reads those to about 256 ns: for display only). Every server message carries
// the session id and a generation; levels, trades and candles are arrays to keep snapshots small:
// level [price, quantity, orders], trade [ts, price, quantity, resting side], candle [start, open, high, low, close,
// volume].

namespace simulator::server {

// The protocol version hello carries; 2 adds days: a UI-1 client ignores the new keys, a UI-2 client needs them.
inline constexpr int ws_protocol = 2;
// The largest message a client may send (spec §6.4).
inline constexpr std::size_t max_message_bytes = 64 * 1024;

enum class ClientType : std::uint8_t {
    Auth,     // auth{token}: the first message when the server has a token
    Control,  // control{action}: play, pause, step n, speed s, seek ts, restart, symbol S [days], cancel_seek
    Command,  // order, cancel, attach{strategy, params} or detach
    Resync    // resync{from_report_seq}
};

// A client message that passed its schema: exactly its keys, each of its type and in range.
struct ClientMessage
{
    ClientType type{ClientType::Auth};
    std::string token{};               // Auth
    std::string session_id{};          // all but Auth
    std::uint64_t generation{0};       // all but Auth
    Control control{};                 // Control
    Command command{};                 // Command, with command.generation set
    std::uint64_t from_report_seq{0};  // Resync
    std::optional<std::uint64_t> client_id{};  // an order's, kept even when the rest of it is refused
};

// False, with `error` saying what is wrong, if `text` is not one client message. A key given twice keeps its last
// value, as JSON.parse does, and the command log records the command as decoded.
bool decode(std::string_view text, ClientMessage& message, std::string& error);

// The role, and the session's price and quantity scales, fees and risk limits.
std::string encode_hello(std::string_view session_id, std::uint64_t generation, bool controller,
                         const SessionConfig& config);
// A snapshot, or with `state` the full authoritative state a (re)connect or a gap starts from; `full_resync`: after
// a gap, so the client's report history is incomplete.
std::string encode_snapshot(std::string_view session_id, const Snapshot& snapshot, bool state, bool full_resync);
// A ring record: report{seq} for a command step, an execution report or a generation's start or end; ack{control,
// state} for an Ack.
std::string encode_record(std::string_view session_id, const Record& record);
// A command's record is in the log: persisted "fsynced" with --fsync, else "flushed".
std::string encode_command_ack(std::string_view session_id, std::uint64_t generation, std::uint64_t command_seq,
                               bool fsynced);
// Reports `from` to `to` were overwritten before the client read them.
std::string encode_gap(std::string_view session_id, std::uint64_t generation, std::uint64_t from, std::uint64_t to);
std::string encode_progress(std::string_view session_id, std::uint64_t generation, Timestamp seek);
// `command_seq` 0: the error is not about a command; `client_id` is the order's, when its message carried one.
std::string encode_error(std::string_view session_id, std::uint64_t generation, std::string_view code,
                         std::string_view message, std::uint64_t command_seq = 0,
                         std::optional<std::uint64_t> client_id = std::nullopt);

// GET /api/symbols.
std::string encode_symbols(std::span<const std::string> symbols);
// GET /api/days: the days discovered, oldest first, and the starting day.
std::string encode_days(std::chrono::year_month_day start, std::span<const DayHandle> days);
// GET /api/history.
std::string encode_history(std::string_view session_id, std::uint64_t generation, std::span<const Candle> candles,
                           std::span<const Trade> trades);

}  // namespace simulator::server
