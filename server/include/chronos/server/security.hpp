#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Spec §6.4: a local tool, still defensive.

namespace simulator::server {

// `bytes` random bytes from std::random_device, as lower-case hex.
std::string random_hex(std::size_t bytes);

// Equal, in a time that does not depend on where `given` first differs from `expected`.
bool token_equal(std::string_view given, std::string_view expected) noexcept;

// The token a server bound to `host` requires: `token` if given, else a fresh 256-bit one for any host but 127.0.0.1,
// else none.
std::string required_token(std::string_view host, std::string token);

// 0 if an /api request may be served. With a token it needs "Authorization: Bearer <token>" (else 401); without one,
// a Host naming this server on the loopback interface (else 403), so a DNS-rebound page cannot reach it.
int api_status(std::string_view host, std::string_view authorization, std::string_view token, std::uint16_t port);

// True if a WebSocket upgrade may proceed: its Origin is the page this server served (http://<Host>) and, without a
// token, that Host names this server on the loopback interface.
bool websocket_allowed(std::string_view origin, std::string_view host, std::string_view token, std::uint16_t port);

}  // namespace simulator::server
