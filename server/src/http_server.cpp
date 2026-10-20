#include "chronos/server/http_server.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crow.h>

#include "chronos/server/protocol.hpp"
#include "chronos/server/security.hpp"
#include "simulator/cli/replay_args.hpp"

namespace simulator::server {
namespace {

Timestamp wall_now() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

crow::response json_response(int code, std::string body)
{
    crow::response response{code, std::move(body)};
    response.set_header("Content-Type", "application/json");
    return response;
}

// The query parameter `key` as a whole number; false if it is missing or anything else.
template <class I>
bool number(const crow::request& request, const char* key, I& value)
{
    const char* text = request.url_params.get(key);
    if (text == nullptr) {
        return false;
    }
    const std::string_view view{text};
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    return error == std::errc{} && end == view.data() + view.size();
}

// A log up to its last newline, so a line the writer is still writing is never served half.
std::string complete_lines(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    std::string text{std::istreambuf_iterator<char>{in}, {}};
    text.resize(text.find_last_of('\n') + 1);  // npos + 1 is 0: no complete line yet
    return text;
}

// A session id from a URL names a file only if it is letters, digits, '-' and '_'.
bool plain_id(std::string_view id)
{
    constexpr std::string_view allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    return !id.empty() && id.size() <= 64 && id.find_first_not_of(allowed) == std::string_view::npos;
}

}  // namespace

struct HttpServer::Impl
{
    Hub& hub;
    ServerOptions options;
    crow::SimpleApp app;
    std::mutex mutex;  // the hub and every peer: each Peer call, its construction and its destruction
    std::unordered_map<crow::websocket::connection*, std::unique_ptr<Peer>> peers;
    std::shared_future<void> running;  // wait() and stop() may be on different threads
    std::atomic<bool> stopped{false};  // the first stop() wins, from any thread

    Impl(Hub& h, ServerOptions o)
        : hub(h), options(std::move(o))
    {
        routes();
    }

    void routes();
    crow::response api(const crow::request& request, const std::function<crow::response()>& serve);
    crow::response change(const crow::request& request, const std::function<crow::response()>& serve);
    crow::response submit(const crow::request& request);
    crow::response symbols(const crow::request& request);
    crow::response history(const crow::request& request);
    crow::response static_file(std::string path);
    void pump();
    static void deliver(crow::websocket::connection& connection, std::vector<std::string>& out, bool keep);
};

void HttpServer::Impl::routes()
{
    static std::once_flag quiet;  // Crow's log level is one per process: set before any server's threads read it
    std::call_once(quiet, [] { crow::logger::setLogLevel(crow::LogLevel::Warning); });
    // ponytail: a rescan that finds a new file hashes it here, seconds on one of 4 Crow threads; scan in the
    // background if that stalls the UI.
    CROW_ROUTE(app, "/api/days")([this](const crow::request& request) {
        return api(request, [this] {
            const std::vector<DayHandle> days = hub.library != nullptr ? hub.library->scan() : std::vector<DayHandle>{};
            return json_response(200, encode_days(hub.start, days));
        });
    });
    CROW_ROUTE(app, "/api/symbols")(
        [this](const crow::request& request) { return api(request, [&] { return symbols(request); }); });
    CROW_ROUTE(app, "/api/history")(
        [this](const crow::request& request) { return api(request, [&] { return history(request); }); });
    CROW_ROUTE(app, "/api/sessions/<string>/log")([this](const crow::request& request, const std::string& id) {
        return api(request, [&] {
            const std::filesystem::path path = options.sessions / (id + ".jsonl");
            if (!plain_id(id) || !std::filesystem::is_regular_file(path)) {
                return crow::response{404};
            }
            crow::response response{200, complete_lines(path)};
            response.set_header("Content-Type", "application/x-ndjson");
            return response;
        });
    });
    CROW_ROUTE(app, "/api/strategies")([this](const crow::request& request) {
        return api(request, [] { return json_response(200, encode_strategies()); });
    });
    // The Lab (spec §5.6). Crow drops a connection whose body passes 64 KiB before a route runs.
    CROW_ROUTE(app, "/api/backtests")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)([this](const crow::request& request) {
            if (request.method == crow::HTTPMethod::Post) {
                return change(request, [&] { return submit(request); });
            }
            return api(request, [this] {
                return hub.lab != nullptr ? json_response(200, hub.lab->list()) : crow::response{404};
            });
        });
    CROW_ROUTE(app, "/api/backtests/<string>")
        .methods(crow::HTTPMethod::Get,
                 crow::HTTPMethod::Delete)([this](const crow::request& request, const std::string& id) {
            if (request.method == crow::HTTPMethod::Delete) {
                return change(request, [&] {
                    std::string warning;
                    if (!plain_id(id) || hub.lab->remove(id, warning) == Refusal::NotFound) {
                        return crow::response{404};
                    }
                    return warning.empty() ? crow::response{204} : json_response(200, encode_warning(warning));
                });
            }
            return api(request, [&] {
                std::string run;
                if (hub.lab == nullptr || !plain_id(id) || hub.lab->get(id, run) == Refusal::NotFound) {
                    return crow::response{404};
                }
                return json_response(200, std::move(run));
            });
        });
    CROW_ROUTE(app, "/api/backtests/<string>/cancel")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& request, const std::string& id) {
            return change(request, [&] {
                const Refusal refusal = plain_id(id) ? hub.lab->cancel(id) : Refusal::NotFound;
                return crow::response{refusal == Refusal::NotFound ? 404 : refusal == Refusal::Conflict ? 409 : 204};
            });
        });
    CROW_ROUTE(app, "/api/backtests/<string>/verify")
        .methods(crow::HTTPMethod::Post)([this](const crow::request& request, const std::string& id) {
            return change(request, [&] {
                switch (plain_id(id) && hub.library != nullptr ? hub.lab->verify(id, *hub.library)
                                                               : Refusal::NotFound) {
                case Refusal::None:
                    return json_response(202, encode_run_id(id));
                case Refusal::NotFound:
                    break;
                case Refusal::Conflict:
                    return json_response(409, encode_lab_error("not_completed"));
                case Refusal::InputChanged:
                    return json_response(409, encode_lab_error("input_changed"));
                case Refusal::OtherSettings:
                    return json_response(409, encode_lab_error("other_settings"));
                case Refusal::QueueFull:
                    return json_response(429, encode_lab_error("queue_full"));
                }
                return crow::response{404};
            });
        });
    // Crow drops a connection whose message passes max_message_bytes before onmessage; onopen follows onaccept's allow.
    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .max_payload(max_message_bytes)
        .onaccept([this](const crow::request& request, std::optional<crow::response>& refusal, void**) {
            if (!websocket_allowed(request.get_header_value("Origin"), request.get_header_value("Host"), hub.token,
                                   app.port())) {
                refusal = crow::response{403};
            }
        })
        .onopen([this](crow::websocket::connection& connection) {
            const std::lock_guard lock{mutex};
            peers.emplace(&connection, std::make_unique<Peer>(hub, wall_now()));
        })
        .onmessage([this](crow::websocket::connection& connection, const std::string& text, bool binary) {
            const std::lock_guard lock{mutex};
            const auto peer = peers.find(&connection);
            if (peer == peers.end()) {
                return;
            }
            std::vector<std::string> out;
            const bool keep = !binary && peer->second->receive(text, wall_now(), out);  // the protocol is text
            deliver(connection, out, keep);
            if (!keep) {
                peers.erase(peer);
            }
        })
        .onclose([this](crow::websocket::connection& connection, const std::string&, std::uint16_t) {
            const std::lock_guard lock{mutex};
            peers.erase(&connection);
        });
    CROW_ROUTE(app, "/")([this] { return static_file("index.html"); });
    CROW_ROUTE(app, "/<path>")([this](const std::string& path) { return static_file(path); });
    app.tick(std::chrono::milliseconds{1'000 / 30}, [this] { pump(); });
}

// Serves an /api request that the token, or else the loopback Host, allows.
crow::response HttpServer::Impl::api(const crow::request& request, const std::function<crow::response()>& serve)
{
    const int status =
        api_status(request.get_header_value("Host"), request.get_header_value("Authorization"), hub.token, app.port());
    return status == 0 ? serve() : crow::response{status};
}

// Serves an /api POST or DELETE that api() allows and that comes as JSON from this server's own page (spec §5.6): JSON,
// as no cross-origin form can send it without a preflight, else 415; an Origin of http://<Host>, else 403. Without a
// Lab, 404.
crow::response HttpServer::Impl::change(const crow::request& request, const std::function<crow::response()>& serve)
{
    return api(request, [&] {
        const std::string type = request.get_header_value("Content-Type");
        if (type != "application/json" && !type.starts_with("application/json;")) {
            return crow::response{415};
        }
        if (!websocket_allowed(request.get_header_value("Origin"), request.get_header_value("Host"), hub.token,
                               app.port())) {
            return crow::response{403};
        }
        return hub.lab != nullptr ? serve() : crow::response{404};
    });
}

// POST /api/backtests: a RunSpec over days the library lists, each ITCH 5.0 and listing the symbol; 202 {id}.
crow::response HttpServer::Impl::submit(const crow::request& request)
{
    RunSpec spec;
    std::string field;
    if (!decode_run_spec(request.body, spec, field)) {
        return json_response(400, encode_lab_error("bad_request", field));
    }
    std::vector<DayHandle> days;
    std::vector<std::string> missing;  // the days whose stock directory lacks the symbol
    for (std::size_t i = 0; i < spec.days.count; ++i) {
        DayHandle day = hub.library != nullptr ? hub.library->find(spec.days.day[i]) : nullptr;
        if (day == nullptr || !day->itch50) {
            return json_response(400, encode_lab_error("bad_request", "days[" + std::to_string(i) + "]"));
        }
        if (!std::binary_search(day->symbols.begin(), day->symbols.end(), name(spec.ticker))) {
            missing.push_back(iso_date(day->date));
        }
        days.push_back(std::move(day));
    }
    if (!missing.empty()) {
        return json_response(400, encode_lab_error("symbol_missing", {}, missing));
    }
    std::string id;
    if (hub.lab->submit(spec, std::move(days), id) == Refusal::QueueFull) {
        return json_response(429, encode_lab_error("queue_full"));
    }
    return json_response(202, encode_run_id(id));
}

// GET /api/symbols[?days=YYYY-MM-DD,...]: the tickers in the stock directory of every day listed; without the list,
// the starting day's.
crow::response HttpServer::Impl::symbols(const crow::request& request)
{
    const char* listed = request.url_params.get("days");
    if (listed == nullptr) {
        const DayHandle start = hub.library != nullptr ? hub.library->find(hub.start) : nullptr;
        return json_response(200, encode_symbols(start != nullptr ? start->symbols : std::vector<std::string>{}));
    }
    // ponytail: only the 80 KB header limit bounds the list; cap it at max_days if a caller abuses it.
    const std::string_view days{listed};
    std::vector<std::string> common;
    for (std::size_t at = 0; at <= days.size();) {
        const std::size_t end = std::min(days.find(',', at), days.size());
        std::chrono::year_month_day date{};
        const DayHandle day =
            hub.library != nullptr && parse_date(days.substr(at, end - at), date) ? hub.library->find(date) : nullptr;
        if (day == nullptr) {
            return json_response(400, R"({"error":"unknown_day"})");
        }
        if (at == 0) {
            common = day->symbols;
        } else {
            std::vector<std::string> both;
            std::set_intersection(common.begin(), common.end(), day->symbols.begin(), day->symbols.end(),
                                  std::back_inserter(both));
            common = std::move(both);
        }
        at = end + 1;
    }
    return json_response(200, encode_symbols(common));
}

// GET /api/history?session_id&generation&from_ts: the generation's bars and latest trades, as of its last snapshot.
// ponytail: a read holds the history lock about 1-3 ms per day of the chain, and may drop snapshot frames for all;
// per-client rate limiting or paging comes when needed.
crow::response HttpServer::Impl::history(const crow::request& request)
{
    const char* session_id = request.url_params.get("session_id");
    std::uint64_t generation = 0;
    Timestamp from_ts = 0;
    if (session_id == nullptr || !number(request, "generation", generation) || !number(request, "from_ts", from_ts)) {
        return json_response(400, R"({"error":"bad_request"})");
    }
    if (session_id != hub.session_id) {
        return json_response(404, R"({"error":"unknown_session"})");
    }
    std::vector<Candle> candles;
    std::vector<Trade> trades;
    // False: the history holds another generation, or none of this one is published yet (an empty answer, not stale).
    if (!hub.session.read_history(generation, from_ts, candles, trades)) {
        const std::uint64_t current = hub.session.generation();
        if (generation != current) {
            return json_response(409, R"({"error":"stale_generation","generation":)" + std::to_string(current) + "}");
        }
    }
    return json_response(200, encode_history(hub.session_id, generation, candles, trades));
}

// A file of the built UI; public, as it holds no secrets.
crow::response HttpServer::Impl::static_file(std::string path)
{
    crow::utility::sanitize_filename(path);
    crow::response response;
    response.set_static_file_info_unsafe((options.web / path).string());
    if (response.code == 404) {
        return crow::response{404, "no such file (the UI is built into web/dist)"};
    }
    return response;
}

// Once a frame, on Crow's main thread: polls every connection, so one that never authenticates times out.
void HttpServer::Impl::pump()
{
    const std::lock_guard lock{mutex};
    const Timestamp now = wall_now();
    std::vector<std::string> out;
    for (auto peer = peers.begin(); peer != peers.end();) {
        out.clear();
        const bool keep = peer->second->poll(now, out);
        deliver(*peer->first, out, keep);
        peer = keep ? std::next(peer) : peers.erase(peer);
    }
}

// Sends under the hub lock, so the connection outlives the call; every close has one code and reason.
// ponytail: Crow exposes no per-connection send backlog, so a client that stops reading its socket buffers its frames
// in Crow (about 1 MB/s at 30 snapshots/s); a write-completion hook would let the pump hold it to the latest snapshot.
void HttpServer::Impl::deliver(crow::websocket::connection& connection, std::vector<std::string>& out, bool keep)
{
    for (std::string& text : out) {
        connection.send_text(std::move(text));
    }
    if (!keep) {
        // ponytail: Crow keeps a closed socket until the client answers the close, so a silent client holds its fd; a
        // Crow patch that shuts the socket a few seconds after close() bounds it.
        connection.close("closed by the server", crow::websocket::CloseStatusCode::PolicyViolated);
    }
}

HttpServer::HttpServer(Hub& hub, ServerOptions options)
    : impl_(std::make_unique<Impl>(hub, std::move(options)))
{}

HttpServer::~HttpServer()
{
    stop();
}

// ponytail: a non-loopback bind guards trading, not availability: Crow writes HTTP responses with a blocking write
// and keeps slow or silent sockets; put a buffering reverse proxy in front, or move to async writes.
bool HttpServer::start()
{
    impl_->app.bindaddr(impl_->options.host).port(impl_->options.port).concurrency(4);
    impl_->running = impl_->app.run_async().share();
    // A failed bind still reports a started server, but never a bound one.
    return impl_->app.wait_for_server_start() == std::cv_status::no_timeout && impl_->app.is_bound();
}

std::uint16_t HttpServer::port() const
{
    return impl_->app.port();
}

void HttpServer::wait()
{
    if (impl_->running.valid()) {
        impl_->running.wait();
    }
}

void HttpServer::stop()
{
    if (!impl_->running.valid() || impl_->stopped.exchange(true)) {
        return;
    }
    impl_->app.stop();
    impl_->running.wait();
    const std::lock_guard lock{impl_->mutex};
    impl_->peers.clear();  // the connections are Crow's, and its threads are gone
}

}  // namespace simulator::server
