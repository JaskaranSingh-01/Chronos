#include "chronos/server/security.hpp"

#include <random>

namespace simulator::server {
namespace {

// True if a Host header names this server on the loopback interface.
bool loopback_host(std::string_view host, std::uint16_t port)
{
    const std::string suffix = ":" + std::to_string(port);
    return host == "127.0.0.1" + suffix || host == "localhost" + suffix;
}

}  // namespace

std::string random_hex(std::size_t bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::random_device device{"/dev/urandom"};
    std::string hex;
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned byte = device() & 0xFFU;
        hex += digits[byte >> 4];
        hex += digits[byte & 0xFU];
    }
    return hex;
}

bool token_equal(std::string_view given, std::string_view expected) noexcept
{
    if (expected.empty()) {
        return false;
    }
    unsigned char differ = given.size() == expected.size() ? 0 : 1;
    for (std::size_t i = 0; i < given.size(); ++i) {
        differ |= static_cast<unsigned char>(given[i] ^ expected[i % expected.size()]);
    }
    return differ == 0;
}

std::string required_token(std::string_view host, std::string token)
{
    if (token.empty() && host != "127.0.0.1") {
        token = random_hex(32);
    }
    return token;
}

int api_status(std::string_view host, std::string_view authorization, std::string_view token, std::uint16_t port)
{
    constexpr std::string_view bearer = "Bearer ";
    if (!token.empty()) {
        return authorization.starts_with(bearer) && token_equal(authorization.substr(bearer.size()), token) ? 0 : 401;
    }
    return loopback_host(host, port) ? 0 : 403;
}

bool websocket_allowed(std::string_view origin, std::string_view host, std::string_view token, std::uint16_t port)
{
    return !host.empty() && origin == "http://" + std::string{host} && (!token.empty() || loopback_host(host, port));
}

}  // namespace simulator::server
