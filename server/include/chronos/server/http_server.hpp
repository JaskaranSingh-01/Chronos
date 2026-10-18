#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "chronos/server/peer.hpp"

namespace simulator::server {

struct ServerOptions
{
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};                    // 0: any free port
    std::filesystem::path web{"web/dist"};       // the static UI; while it is missing, / answers 404
    std::filesystem::path sessions{"sessions"};  // the command logs, <session_id>.jsonl
};

// chronos-serve's HTTP and WebSocket endpoints (spec §6.2) on Crow's threads; every frame it polls each connection.
class HttpServer
{
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    // `hub` outlives the server.
    HttpServer(Hub& hub, ServerOptions options);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds and serves on its own threads; false if the address cannot be bound.
    bool start();
    // The bound port, once started.
    std::uint16_t port() const;
    // Blocks until stop(), SIGINT or SIGTERM.
    void wait();
    // Stops serving and joins the server's threads; the hub's peers are gone after it, its sockets once it is
    // destroyed.
    void stop();
};

}  // namespace simulator::server
