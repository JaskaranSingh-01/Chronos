#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "chronos/server/command_log.hpp"
#include "chronos/server/http_server.hpp"
#include "chronos/server/protocol.hpp"
#include "chronos/server/session_runner.hpp"
#include "test_support.hpp"

namespace simulator::server::test {
namespace {

using namespace std::chrono_literals;
using nlohmann::json;

// A blocking loopback client of the server under test: HTTP requests, or one WebSocket as a browser opens it.
class Client
{
private:
    int fd_{-1};
    std::string buffer_;  // read, not yet consumed
    bool closed_{false};  // the server closed the connection
    std::string close_;   // the payload of its close frame: the status code, then the reason

    // Reads what arrives within `timeout`; false on a timeout or once the server has closed.
    bool fill(std::chrono::milliseconds timeout)
    {
        const timeval wait{.tv_sec = static_cast<time_t>(timeout.count() / 1'000),
                           .tv_usec = static_cast<suseconds_t>(timeout.count() % 1'000 * 1'000)};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        char chunk[65'536];
        const ssize_t got = ::recv(fd_, chunk, sizeof chunk, 0);
        if (got > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(got));
            return true;
        }
        closed_ = got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
        return false;
    }

public:
    // Connects to the server; a small `receive_buffer` (bytes) makes a client that stops reading back up sooner.
    explicit Client(std::uint16_t port, int receive_buffer = 0)
        : fd_(::socket(AF_INET, SOCK_STREAM, 0))
    {
        std::signal(SIGPIPE, SIG_IGN);  // a write to a connection the server closed must not end the test
        if (receive_buffer > 0) {
            ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof receive_buffer);
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
    }

    ~Client()
    {
        ::close(fd_);
    }

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Sends raw bytes, as far as the server has not closed.
    void write(const std::string& bytes)
    {
        for (std::size_t sent = 0; sent < bytes.size();) {
            const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
            if (n <= 0) {
                return;  // the server closed it
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    // Sends an HTTP request and reads its response: the status, and the body up to its Content-Length.
    std::pair<int, std::string> request(const std::string& text)
    {
        write(text);
        std::size_t end = std::string::npos;
        while ((end = buffer_.find("\r\n\r\n")) == std::string::npos && fill(5s)) {
        }
        int status = 0;
        std::size_t length = 0;
        std::sscanf(buffer_.c_str(), "HTTP/1.1 %d", &status);
        if (const std::size_t field = buffer_.find("Content-Length: "); field != std::string::npos && field < end) {
            length = std::stoul(buffer_.substr(field + 16));
        }
        while (end != std::string::npos && buffer_.size() < end + 4 + length && fill(5s)) {
        }
        const std::string body = end == std::string::npos ? std::string{} : buffer_.substr(end + 4, length);
        buffer_.clear();
        return {status, body};
    }

    // Asks to upgrade /ws to a WebSocket; the response's status, 101 when it is accepted.
    int upgrade(const std::string& origin, const std::string& host)
    {
        write("GET /ws HTTP/1.1\r\nHost: " + host +
              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
              "Sec-WebSocket-Version: 13\r\n" +
              (origin.empty() ? "" : "Origin: " + origin + "\r\n") + "\r\n");
        while (buffer_.find("\r\n\r\n") == std::string::npos && fill(5s)) {
        }
        int status = 0;
        std::sscanf(buffer_.c_str(), "HTTP/1.1 %d", &status);
        const std::size_t end = buffer_.find("\r\n\r\n");
        buffer_.erase(0, end == std::string::npos ? buffer_.size() : end + 4);  // frames may follow at once
        return status;
    }

    // One masked text frame, as browsers send.
    void send(const std::string& text)
    {
        std::string frame{'\x81'};
        const std::size_t size = text.size();
        if (size < 126) {
            frame += static_cast<char>(0x80 | size);
        } else if (size < 65'536) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(size >> 8);
            frame += static_cast<char>(size & 0xFF);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame += static_cast<char>((size >> shift) & 0xFF);
            }
        }
        const char mask[4] = {0x12, 0x34, 0x56, 0x78};
        frame.append(mask, 4);
        for (std::size_t i = 0; i < size; ++i) {
            frame += static_cast<char>(text[i] ^ mask[i % 4]);
        }
        write(frame);
    }

    // The next text message; nothing once the server closes, or if none comes within `timeout`.
    std::optional<std::string> receive(std::chrono::milliseconds timeout = 5s)
    {
        for (;;) {
            const auto byte = [&](std::size_t i) {
                return static_cast<std::size_t>(static_cast<unsigned char>(buffer_[i]));
            };
            std::size_t header = 2;
            std::size_t size = buffer_.size() >= 2 ? byte(1) & 0x7F : 0;
            if (size == 126 && buffer_.size() >= 4) {
                size = byte(2) << 8 | byte(3);
                header = 4;
            } else if (size == 127 && buffer_.size() >= 10) {
                size = 0;
                for (std::size_t i = 2; i < 10; ++i) {
                    size = size << 8 | byte(i);
                }
                header = 10;
            }
            const bool sized = buffer_.size() >= 2 && (size < 126 || header > 2);
            if (sized && buffer_.size() >= header + size) {
                const std::size_t opcode = byte(0) & 0x0F;
                std::string payload = buffer_.substr(header, size);
                buffer_.erase(0, header + size);
                if (opcode == 0x8) {
                    closed_ = true;
                    close_ = payload;
                    return std::nullopt;
                }
                if (opcode == 0x1) {
                    return payload;
                }
                continue;  // a ping or pong
            }
            if (!fill(timeout)) {
                return std::nullopt;
            }
        }
    }

    bool closed() const noexcept
    {
        return closed_;
    }

    // The close frame the server sent, empty if it dropped the connection without one.
    const std::string& close_frame() const noexcept
    {
        return close_;
    }
};

// Messages until one of `type` that `match` accepts; null if the connection closes or none comes within 5 s.
json await(Client& client, const std::string& type, const std::function<bool(const json&)>& match = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::optional<std::string> text = client.receive();
        if (!text) {
            return nullptr;
        }
        const json message = json::parse(*text);
        if (message["type"] == type && (!match || match(message))) {
            return message;
        }
    }
    return nullptr;
}

// A session served on a free loopback port, with its engine thread.
struct Served
{
    Hub hub;
    HttpServer server;
    SessionRunner runner;

    Served(ReplaySession& session, ServerOptions options, std::string token = {}, const LogQueue* log = nullptr,
           const LogWriter* writer = nullptr, DayLibrary* library = nullptr)
        : hub{.session = session,
              .log = log,
              .writer = writer,
              .session_id = "test",
              .token = std::move(token),
              .config = config(),
              .library = library,
              .start = 2019y / 12 / 30},
          server{hub, std::move(options)},
          runner{session}
    {
        EXPECT_TRUE(server.start());
    }

    std::string host() const
    {
        return "127.0.0.1:" + std::to_string(server.port());
    }

    // GET `target` as a page this server served would, or naming another `host`.
    std::pair<int, std::string> get(const std::string& target, const std::string& authorization = "",
                                    const std::string& host = "")
    {
        Client client{server.port()};
        return client.request("GET " + target + " HTTP/1.1\r\nHost: " + (host.empty() ? this->host() : host) + "\r\n" +
                              (authorization.empty() ? "" : "Authorization: " + authorization + "\r\n") +
                              "Connection: close\r\n\r\n");
    }

    // A WebSocket from this server's own page; a test failure if the upgrade is refused.
    std::unique_ptr<Client> connect(int receive_buffer = 0)
    {
        auto client = std::make_unique<Client>(server.port(), receive_buffer);
        EXPECT_EQ(client->upgrade("http://" + host(), host()), 101);
        return client;
    }
};

ServerOptions options(const std::filesystem::path& web = "/nonexistent/chronos-web",
                      const std::filesystem::path& sessions = "/nonexistent/chronos-sessions")
{
    return ServerOptions{.host = "127.0.0.1", .port = 0, .web = web, .sessions = sessions};
}

const std::string kBuy = R"(,"client_id":9,"side":"buy","order_type":"post_only","price":999800,"qty":10)";
// The close frame of every close the server decides on, so a timeout, a wrong token and a bad message look alike.
const std::string kClosedByServer = std::string{"\x03\xF0"} + "closed by the server";

TEST(ServerTest, ServesTheSymbolsHistoryLogAndUi)
{
    std::vector<MarketEvent> events{add(1, 1, Side::Sell, 1'000'000, 1'000)};
    for (Timestamp second = 1; second <= 5; ++second) {
        events.push_back(execute(second * kSecond, 1, 1));  // one trade a second
    }
    ReplaySession session{config(), over(events)};
    const ScratchDir web{"web"};
    const ScratchDir logs{"sessions"};
    std::ofstream{logs.path / "test.jsonl"} << "{\"a\":1}\n{\"b\":2}\n{\"c\":";  // its last line is half written
    const ScratchDir data{"data"};
    itch_test::ItchBuilder day;  // the starting day's stock directory
    day.system_event(0, kSecond, 'O')
        .stock_directory(1, kSecond, "AAA")
        .stock_directory(2, kSecond, "BBB")
        .system_event(0, 2 * kSecond, 'S');
    write_file(data.path / "12302019.NASDAQ_ITCH50.bin", day.bytes());
    DayLibrary library{data.path, data.path / "fingerprints.json"};
    library.scan();
    Served served{session, options(web.path / "dist", logs.path), {}, nullptr, nullptr, &library};
    session.control(kFullSpeed);
    session.control(kPlay);
    ASSERT_TRUE(eventually([&] {
        SessionState published = SessionState::Paused;
        session.read_snapshot([&](const Snapshot& s) { published = s.state; });
        return published == SessionState::Ended;
    }));

    EXPECT_EQ(served.get("/api/symbols"), (std::pair<int, std::string>{200, R"({"symbols":["AAA","BBB"]})"}));
    const auto [status, body] = served.get("/api/history?session_id=test&generation=1&from_ts=0");
    ASSERT_EQ(status, 200);
    const json history = json::parse(body);
    EXPECT_EQ(history["candles"].size(), 4u);  // seconds 1 to 4; the bar of 5 s is still open
    EXPECT_EQ(history["trades"].size(), 5u);
    EXPECT_EQ(served.get("/api/history?session_id=test&generation=2&from_ts=0").first, 409);
    EXPECT_EQ(served.get("/api/history?session_id=other&generation=1&from_ts=0").first, 404);
    EXPECT_EQ(served.get("/api/history?session_id=test&generation=one&from_ts=0").first, 400);
    EXPECT_EQ(served.get("/api/sessions/test/log"), (std::pair<int, std::string>{200, "{\"a\":1}\n{\"b\":2}\n"}));
    EXPECT_EQ(served.get("/api/sessions/no.such/log").first, 404);
    EXPECT_EQ(served.get("/api/symbols", "", "evil.example:" + std::to_string(served.server.port())).first, 403);
    EXPECT_EQ(served.get("/api/days", "", "evil.example:" + std::to_string(served.server.port())).first, 403);

    EXPECT_EQ(served.get("/").first, 404);  // no UI built yet
    std::filesystem::create_directories(web.path / "dist");
    std::ofstream{web.path / "dist" / "index.html"} << "<title>Chronos</title>";
    EXPECT_EQ(served.get("/"), (std::pair<int, std::string>{200, "<title>Chronos</title>"}));
    EXPECT_EQ(served.get("/index.html").first, 200);
    EXPECT_EQ(served.get("/../dist/index.html").first, 404);  // no way out of the UI's directory
}

// GET /api/days rescans on every request; /api/symbols lists the starting day's tickers, or those on every day listed.
TEST(ServerTest, ServesTheDaysAndTheSymbolsOnEveryListedDay)
{
    const ScratchDir data{"days"};
    itch_test::ItchBuilder friday;  // AAA and ACME, and no regular hours of its own
    friday.system_event(0, kSecond, 'O')
        .stock_directory(1, kSecond, "AAA")
        .stock_directory(2, kSecond, "ACME")
        .system_event(0, 2 * kSecond, 'S');
    const itch_test::ItchBuilder monday = acme_day();
    write_file(data.path / "12272019.NASDAQ_ITCH50.bin", friday.bytes());
    write_file(data.path / "12302019.NASDAQ_ITCH50.bin", monday.bytes());
    DayLibrary library{data.path, data.path / "fingerprints.json"};
    library.scan();
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Served served{session, options(), {}, nullptr, nullptr, &library};

    const auto [status, body] = served.get("/api/days");
    ASSERT_EQ(status, 200);
    const json days = json::parse(body);
    EXPECT_EQ(days["start"], "2019-12-30");
    ASSERT_EQ(days["days"].size(), 2u);
    EXPECT_EQ(days["days"][0]["date"], "2019-12-27");
    EXPECT_EQ(days["days"][0]["session"]["source"], "fallback");
    const json& day = days["days"][1];
    const Timestamp midnight = session_midnight_ns(2019y / 12 / 30, -5h);
    EXPECT_EQ(day["date"], "2019-12-30");
    EXPECT_EQ(day["file"], "12302019.NASDAQ_ITCH50.bin");
    EXPECT_EQ(day["size"], monday.bytes().size());
    EXPECT_EQ(day["hash"], day_hash(monday.bytes()));
    EXPECT_EQ(day["quick_check"].get<std::string>().size(), 16u);
    EXPECT_EQ(day["itch50"], true);
    EXPECT_EQ(day["cut_tail"], false);
    EXPECT_EQ(day["session"],
              (json{{"open", midnight + RTH_OPEN_NS}, {"close", midnight + RTH_CLOSE_NS}, {"source", "feed"}}));
    EXPECT_EQ(day["symbols"], 1);

    using Reply = std::pair<int, std::string>;
    EXPECT_EQ(served.get("/api/symbols"), (Reply{200, R"({"symbols":["ACME"]})"}));
    EXPECT_EQ(served.get("/api/symbols?days=2019-12-27"), (Reply{200, R"({"symbols":["AAA","ACME"]})"}));
    EXPECT_EQ(served.get("/api/symbols?days=2019-12-27,2019-12-30"), (Reply{200, R"({"symbols":["ACME"]})"}));
    EXPECT_EQ(served.get("/api/symbols?days=2019-12-27,2019-12-31"), (Reply{400, R"({"error":"unknown_day"})"}));
    EXPECT_EQ(served.get("/api/symbols?days=2019-12-27,").first, 400);
    EXPECT_EQ(served.get("/api/symbols?days=30-12-2019").first, 400);
    EXPECT_EQ(served.get("/api/symbols?days=").first, 400);

    write_file(data.path / "12312019.NASDAQ_ITCH50.bin", monday.bytes());
    const json rescanned = json::parse(served.get("/api/days").second)["days"];
    ASSERT_EQ(rescanned.size(), 3u);
    EXPECT_EQ(rescanned[2]["hash"], day_hash(monday.bytes()));  // hashed in the request that found it
}

TEST(ServerTest, AWebSocketFromAnotherOriginIsRefused)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    const std::string port = std::to_string(served.server.port());
    EXPECT_EQ(Client{served.server.port()}.upgrade("http://evil.example", served.host()), 403);
    EXPECT_EQ(Client{served.server.port()}.upgrade("", served.host()), 403);
    EXPECT_EQ(Client{served.server.port()}.upgrade("http://evil.example:" + port, "evil.example:" + port), 403);
    const std::unique_ptr<Client> own = served.connect();
    const json hello = await(*own, "hello");
    EXPECT_EQ(hello["role"], "controller");
    EXPECT_EQ(hello["limits"]["max_position"], 1'000);  // the session's config, for the ticket's checks
}

TEST(ServerTest, AMessageOverSixtyFourKibibytesClosesTheConnection)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    const std::unique_ptr<Client> client = served.connect();
    ASSERT_FALSE(await(*client, "hello").is_null());

    const std::string prefix = R"({"type":"auth","token":")";
    const std::string at_limit = prefix + std::string(max_message_bytes - prefix.size() - 2, 'x') + R"("})";
    client->send(at_limit);  // read and answered: a second auth is a bad request
    EXPECT_EQ(await(*client, "error")["code"], "bad_request");
    client->send(at_limit + " ");
    while (client->receive()) {
    }
    EXPECT_TRUE(client->closed());

    // A 10-byte fragment, then a continuation claiming 2^64 - 5 bytes: the sum must not wrap past the limit.
    const std::unique_ptr<Client> wrap = served.connect();
    wrap->write(std::string{"\x01\x8A\0\0\0\0"
                            "0123456789"
                            "\x80\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFB\0\0\0\0",
                            30});
    while (wrap->receive(1s)) {
    }
    EXPECT_TRUE(wrap->closed());

    // The protocol is text: a binary frame, here an empty one, closes the connection rather than get bad_request.
    const std::unique_ptr<Client> binary = served.connect();
    binary->write(std::string{"\x82\x80\0\0\0\0", 6});
    while (binary->receive()) {
    }
    EXPECT_TRUE(binary->closed());
    EXPECT_EQ(binary->close_frame(), kClosedByServer);

    // No route takes a body: a 100 KB one aborts the parse, and the server still serves.
    Client http{served.server.port()};
    const std::string body(100'000, 'x');
    EXPECT_NE(http.request("GET /api/symbols HTTP/1.1\r\nHost: " + served.host() +
                           "\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n" + body)
                  .first,
              200);
    EXPECT_EQ(served.get("/api/symbols").first, 200);
}

TEST(ServerTest, ATokenGuardsHttpAndTheWebSocketsFirstMessage)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Served served{session, options(), "t0k3n"};
    EXPECT_EQ(served.get("/api/symbols").first, 401);
    EXPECT_EQ(served.get("/api/symbols", "Bearer t0k3m").first, 401);
    EXPECT_EQ(served.get("/api/symbols", "Bearer t0k3n").first, 200);
    EXPECT_EQ(served.get("/api/days").first, 401);
    EXPECT_EQ(served.get("/").first, 404);  // static files need no token; this server has no UI

    for (const std::string& first :
         {std::string{R"({"type":"auth","token":"t0k3m"})"}, client_message("control", R"(,"action":"play")")}) {
        const std::unique_ptr<Client> client = served.connect();
        client->send(first);
        EXPECT_FALSE(client->receive().has_value()) << first;
        EXPECT_TRUE(client->closed()) << first;
        EXPECT_EQ(client->close_frame(), kClosedByServer) << first;
    }
    const std::unique_ptr<Client> right = served.connect();
    right->send(R"({"type":"auth","token":"t0k3n"})");
    EXPECT_EQ(await(*right, "hello")["role"], "controller");

    // Silence: the server closes the connection 5 s after it opened.
    const auto opened = std::chrono::steady_clock::now();
    const std::unique_ptr<Client> silent = served.connect();
    EXPECT_FALSE(silent->receive(10s).has_value());
    EXPECT_TRUE(silent->closed());
    EXPECT_EQ(silent->close_frame(), kClosedByServer);
    EXPECT_GE(std::chrono::steady_clock::now() - opened, 4s);
}

TEST(ServerTest, TheSecondConnectionIsAViewerAndGetsNotController)
{
    const std::vector<MarketEvent> events = spaced_bids(10);
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    const std::unique_ptr<Client> first = served.connect();
    EXPECT_EQ(await(*first, "hello")["role"], "controller");
    const std::unique_ptr<Client> second = served.connect();
    EXPECT_EQ(await(*second, "hello")["role"], "viewer");
    second->send(client_message("order", kBuy));
    EXPECT_EQ(await(*second, "error")["code"], "not_controller");
    first->send(client_message("order", kBuy));
    EXPECT_FALSE(await(*first, "report", [](const json& m) { return m["step"] == "applied"; }).is_null());
}

TEST(ServerTest, AReconnectResumesFromTheRingOrResyncsAfterAnOverflow)
{
    const std::vector<MarketEvent> events = spaced_bids(100);
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    std::uint64_t seen = 0;  // the last report seq the first connection got
    {
        const std::unique_ptr<Client> before = served.connect();
        ASSERT_FALSE(await(*before, "state").is_null());
        before->send(client_message("order", kBuy));
        const json applied = await(*before, "report", [](const json& m) { return m["step"] == "applied"; });
        ASSERT_FALSE(applied.is_null());
        seen = applied["seq"];
    }
    session.submit(post_only_buy(999'700, 10), 0);  // while it is away
    session.submit(post_only_buy(999'600, 10), 0);

    const std::unique_ptr<Client> after = served.connect();
    ASSERT_FALSE(await(*after, "state").is_null());
    after->send(client_message("resync", R"(,"from_report_seq":)" + std::to_string(seen + 1)));
    const json missed = await(*after, "report");
    ASSERT_FALSE(missed.is_null());
    EXPECT_EQ(missed["seq"], seen + 1);

    while (session.reports_since(0, {}).last_seq <= seen + ReplaySession::ring_capacity) {
        session.submit(cancel(1), 0);  // two records each; the first ones are overwritten
    }
    after->send(client_message("resync", R"(,"from_report_seq":1)"));
    const json gap = await(*after, "gap", [](const json& m) { return m["from"] == 1; });
    ASSERT_FALSE(gap.is_null());
    EXPECT_GT(gap["to"], seen);
    const json state = await(*after, "state");
    ASSERT_FALSE(state.is_null());
    EXPECT_EQ(state["full_resync"], true);
}

// The reconnect may land during the seek or just after it; either way the client ends on generation 2.
TEST(ServerTest, AReconnectAcrossARestartingSeekEndsOnTheNewGenerationWithNoStaleCommand)
{
    const std::vector<MarketEvent> events = spaced_bids(15'000);
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    {
        const std::unique_ptr<Client> client = served.connect();
        ASSERT_FALSE(await(*client, "state").is_null());
        client->send(client_message("control", R"(,"action":"seek","ts":12000000000)"));
        ASSERT_FALSE(await(*client, "snapshot", [](const json& m) { return m["event_seq"] == 12'000; }).is_null());
        client->send(client_message("control", R"(,"action":"seek","ts":6000000000)"));  // backward: generation 2
        client->send(client_message("order", kBuy));  // still for generation 1
    }
    const std::unique_ptr<Client> again = served.connect();
    const json state = await(*again, "state", [](const json& m) { return m["generation"] == 2; });
    ASSERT_FALSE(state.is_null());
    EXPECT_EQ(state["event_seq"], 6'000);

    bool stale = false;
    for (const Record& r : records(session)) {
        EXPECT_FALSE(r.kind == RecordKind::Lifecycle && r.step == Lifecycle::Applied && r.generation == 2);
        stale = stale || (r.kind == RecordKind::Lifecycle && r.reason == ErrorCode::StaleGeneration);
    }
    EXPECT_TRUE(stale);
}

TEST(ServerTest, AClientThatStopsReadingNeverBlocksTheEngineOrOtherClients)
{
    std::vector<MarketEvent> events{add(1, 1, Side::Sell, 1'000'000, 1'000'000)};
    for (Timestamp ms = 1; ms <= 2'000; ++ms) {
        events.push_back(execute(ms * kMs, 1, 1));  // a trade every millisecond for 2 s
    }
    ReplaySession session{config(), over(events)};
    Served served{session, options()};
    const std::unique_ptr<Client> stalled = served.connect(4'096);  // never read
    const std::unique_ptr<Client> reader = served.connect();
    ASSERT_FALSE(await(*reader, "state").is_null());

    const auto start = std::chrono::steady_clock::now();
    session.control(kPlay);  // at 1x: two seconds
    std::size_t snapshots = 0;
    const json ended = await(*reader, "snapshot", [&](const json& m) {
        ++snapshots;
        return m["state"] == "ended";
    });
    ASSERT_FALSE(ended.is_null());
    EXPECT_EQ(ended["event_seq"], 2'001);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s);
    EXPECT_GE(snapshots, 30u);  // at least 15 a second throughout
}

TEST(ServerTest, AnOrderIsAckedOnceLoggedAndItsFillIsReported)
{
    const std::vector<MarketEvent> events{add(100, 1, Side::Sell, 1'000'000, 100),
                                          add(5'000, 2, Side::Buy, 999'900, 100)};
    const Scratch scratch{"server-ack"};
    LogQueue queue;
    const LogWriter writer{queue, scratch.path, config(), false};
    ReplaySession session{config(), over(events), &queue};
    Served served{session, options(), {}, &queue, &writer};
    const std::unique_ptr<Client> client = served.connect();
    ASSERT_FALSE(await(*client, "state").is_null());

    client->send(client_message("control", R"(,"action":"step","n":1)"));  // the ask rests at 100 ns
    ASSERT_FALSE(await(*client, "snapshot", [](const json& m) { return m["event_seq"] == 1; }).is_null());
    client->send(client_message("order", R"(,"client_id":5,"side":"buy","order_type":"ioc","price":1000000,"qty":10)"));
    const json ack = await(*client, "ack", [](const json& m) { return m.contains("command_seq"); });
    ASSERT_FALSE(ack.is_null());
    EXPECT_EQ(ack["persisted"], "flushed");
    client->send(client_message("control", R"(,"action":"step","n":1)"));  // the order arrives before the next event
    const json fill = await(*client, "report", [](const json& m) { return m["report_type"] == "fill"; });
    ASSERT_FALSE(fill.is_null());
    EXPECT_EQ(fill["quantity"], 10);
    EXPECT_EQ(fill["price"], 1'000'000);
}

}  // namespace
}  // namespace simulator::server::test
