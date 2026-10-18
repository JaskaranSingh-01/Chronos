#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "chronos/server/command_log.hpp"
#include "chronos/server/http_server.hpp"
#include "chronos/server/itch_input.hpp"
#include "chronos/server/security.hpp"
#include "chronos/server/session_runner.hpp"
#include "simulator/cli/replay_args.hpp"
#include "simulator/io/mapped_file.hpp"

namespace {

using namespace simulator;
using namespace simulator::server;

constexpr std::string_view serve_usage =
    "usage: chronos-serve --data DIR --date YYYY-MM-DD [--host 127.0.0.1] [--port 8080] [--web web/dist]\n"
    "                     [--token T] [--fsync] [--allow-truncated-tail]\n"
    "\n"
    "Serves an interactive replay of DIR/MMDDYYYY.NASDAQ_ITCH50.bin; the UI picks the symbol.\n"
    "  --host IP               the address to bind, an IP address (default 127.0.0.1); any other requires a\n"
    "                          token, and one is made and printed once if --token is not given; it guards\n"
    "                          trading, not availability: put a buffering reverse proxy in front\n"
    "  --port N                default 8080\n"
    "  --web DIR               the built UI (default web/dist)\n"
    "  --token T               /api needs 'Authorization: Bearer T'; a WebSocket's first message is auth{T}\n"
    "  --fsync                 sync each command line of the log (sessions/<id>.jsonl) to the device\n"
    "  --allow-truncated-tail  a file cut mid-message ends there instead of failing\n"
    "\n"
    "exit status: 0 after SIGINT or SIGTERM; 2 bad usage, a data file that cannot be opened or has no stock\n"
    "directory, a log that cannot be written, or an address that cannot be bound\n";

struct ServeArgs
{
    std::filesystem::path data;
    std::chrono::year_month_day date{};
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::filesystem::path web{"web/dist"};
    std::string token;
    bool fsync{false};
    bool allow_truncated_tail{false};
    bool help{false};
};

// False, with `error` saying why, on an unknown option, a missing or bad value, or no --data or --date.
bool parse(std::span<const std::string_view> args, ServeArgs& out, std::string& error)
{
    bool has_data = false;
    bool has_date = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        if (flag == "-h" || flag == "--help") {
            out.help = true;
            return true;
        }
        if (flag == "--fsync" || flag == "--allow-truncated-tail") {
            (flag == "--fsync" ? out.fsync : out.allow_truncated_tail) = true;
            continue;
        }
        const bool takes_value = flag == "--data" || flag == "--date" || flag == "--host" || flag == "--port" ||
            flag == "--web" || flag == "--token";
        if (!takes_value) {
            error = "unknown option " + std::string{flag};
            return false;
        }
        if (i + 1 == args.size() || args[i + 1].empty()) {
            error = std::string{flag} + " needs a value";
            return false;
        }
        const std::string_view value = args[++i];
        if (flag == "--data") {
            out.data = value;
            has_data = true;
        } else if (flag == "--date") {
            has_date = parse_date(value, out.date);
            if (!has_date) {
                error = "--date: not a YYYY-MM-DD date";
                return false;
            }
        } else if (flag == "--port") {
            const auto [end, failed] = std::from_chars(value.data(), value.data() + value.size(), out.port);
            if (failed != std::errc{} || end != value.data() + value.size()) {
                error = "--port: not a port number";
                return false;
            }
        } else if (flag == "--host") {
            out.host = value;
        } else if (flag == "--web") {
            out.web = value;
        } else {
            out.token = value;
        }
    }
    if (!has_data || !has_date) {
        error = "--data and --date are required";
        return false;
    }
    return true;
}

// NASDAQ's name for a day's file, e.g. 12302019.NASDAQ_ITCH50.bin.
std::string day_file(std::chrono::year_month_day date)
{
    char name[40];
    std::snprintf(name, sizeof name, "%02u%02u%04d.NASDAQ_ITCH50.bin", unsigned{date.month()}, unsigned{date.day()},
                  int{date.year()});
    return name;
}

// The start time in UTC and 64 random bits: never an earlier session's id, whose log this one would append to.
std::string new_session_id()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    ::gmtime_r(&now, &utc);
    char stamp[24];
    std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc);
    return std::string{stamp} + "-" + random_hex(8);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    ServeArgs parsed;
    std::string error;
    if (!parse(args, parsed, error)) {
        std::fprintf(stderr, "error: %s\nrun 'chronos-serve --help' for the options\n", error.c_str());
        return 2;
    }
    if (parsed.help) {
        std::fwrite(serve_usage.data(), 1, serve_usage.size(), stdout);
        return 0;
    }
    const std::filesystem::path path = parsed.data / day_file(parsed.date);
    MappedFile file;
    if (!file.open(path.c_str())) {
        std::fprintf(stderr, "error: %s\n", file.error());
        return 2;
    }
    std::vector<std::string> symbols = symbol_directory(file.bytes());
    if (symbols.empty()) {
        std::fprintf(stderr, "error: %s: no stock directory\n", path.c_str());
        return 2;
    }
    const std::chrono::minutes utc_offset = us_eastern_utc_offset(parsed.date);
    const std::string token = required_token(parsed.host, parsed.token);
    const std::string session_id = new_session_id();
    const std::filesystem::path sessions{"sessions"};
    std::error_code ignored;
    std::filesystem::create_directories(sessions, ignored);

    // The interactive simulator: the backtest's 50 us latencies, no fees, and limits for trading by hand. The log's
    // header records all of it, with the input's fingerprint.
    const SessionConfig config{
        .session_id = session_id,
        .input = input_fingerprint(path.filename().string(), file.bytes()),
        .instrument = {.price_scale = 4, .quantity_scale = 0},
        .latency = {.order_latency = 50'000, .report_latency = 50'000},
        .limits = {.max_position = 10'000, .max_order_quantity = Quantity{1'000}, .max_open_orders = 16},
        .options = {.session_midnight = session_midnight_ns(parsed.date, utc_offset)},
        .source_size = file.bytes().size()};
    // Declared in this order, so destroyed writer last: the server, then the engine thread, before the session.
    LogQueue queue;
    const std::filesystem::path log_path = sessions / (session_id + ".jsonl");
    const LogWriter writer{queue, log_path, config, parsed.fsync};
    if (queue.failed()) {
        std::fprintf(stderr, "error: cannot write the log %s\n", log_path.c_str());
        return 2;
    }
    ReplaySession session{config, itch_factory(file.bytes(), parsed.date, utc_offset, parsed.allow_truncated_tail),
                          &queue};
    std::optional<SessionRunner> runner{std::in_place, session};
    Hub hub{.session = session,
            .log = &queue,
            .writer = &writer,
            .session_id = session_id,
            .token = token,
            .fsync = parsed.fsync,
            .config = config,
            .symbols = std::move(symbols)};
    HttpServer server{hub,
                      ServerOptions{.host = parsed.host, .port = parsed.port, .web = parsed.web, .sessions = sessions}};
    if (!server.start()) {
        std::fprintf(stderr, "error: cannot serve on %s port %u\n", parsed.host.c_str(), unsigned{parsed.port});
        return 2;
    }
    const std::string shown = parsed.host.find(':') == std::string::npos ? parsed.host : "[" + parsed.host + "]";
    std::printf("chronos-serve: http://%s:%u/ with %zu symbols; session %s, log %s\n", shown.c_str(),
                unsigned{server.port()}, hub.symbols.size(), session_id.c_str(), log_path.c_str());
    if (parsed.token.empty() && !token.empty()) {
        std::printf("token (shown once): %s\n", token.c_str());
    }
    std::fflush(stdout);

    server.wait();  // until SIGINT or SIGTERM
    server.stop();
    runner.reset();
    session.close();  // the log's trailer: where the session stopped, and its digests
    return 0;
}
