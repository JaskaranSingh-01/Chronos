#include <gtest/gtest.h>

#include <string>

#include "chronos/server/security.hpp"

namespace simulator::server {
namespace {

TEST(SecurityTest, TokensCompareWholeAndANonLoopbackHostAlwaysHasOne)
{
    EXPECT_TRUE(token_equal("t0k3n", "t0k3n"));
    EXPECT_FALSE(token_equal("t0k3m", "t0k3n"));
    EXPECT_FALSE(token_equal("t0k3", "t0k3n"));  // a prefix
    EXPECT_FALSE(token_equal("t0k3nn", "t0k3n"));
    EXPECT_FALSE(token_equal("", ""));  // no token configured never matches
    EXPECT_FALSE(token_equal("x", ""));

    const std::string token = random_hex(32);
    EXPECT_EQ(token.size(), 64u);  // 256 bits
    EXPECT_EQ(token.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_NE(random_hex(32), token);

    EXPECT_EQ(required_token("127.0.0.1", ""), "");
    EXPECT_EQ(required_token("127.0.0.1", "mine"), "mine");
    EXPECT_EQ(required_token("0.0.0.0", "mine"), "mine");
    EXPECT_EQ(required_token("0.0.0.0", "").size(), 64u);
    EXPECT_EQ(required_token("192.168.1.5", "").size(), 64u);
}

TEST(SecurityTest, ApiRequestsNeedTheBearerTokenOrThisLoopbackHost)
{
    EXPECT_EQ(api_status("127.0.0.1:8080", "", "", 8080), 0);
    EXPECT_EQ(api_status("localhost:8080", "", "", 8080), 0);
    EXPECT_EQ(api_status("127.0.0.1:9999", "", "", 8080), 403);
    EXPECT_EQ(api_status("evil.example:8080", "", "", 8080), 403);  // a DNS-rebound page

    EXPECT_EQ(api_status("box.lan:8080", "Bearer t0k3n", "t0k3n", 8080), 0);
    EXPECT_EQ(api_status("box.lan:8080", "Bearer t0k3m", "t0k3n", 8080), 401);
    EXPECT_EQ(api_status("box.lan:8080", "t0k3n", "t0k3n", 8080), 401);
    EXPECT_EQ(api_status("box.lan:8080", "bearer t0k3n", "t0k3n", 8080), 401);
    EXPECT_EQ(api_status("127.0.0.1:8080", "", "t0k3n", 8080), 401);  // a token binds loopback too
}

TEST(SecurityTest, AWebSocketMustComeFromThisServersOwnPage)
{
    EXPECT_TRUE(websocket_allowed("http://127.0.0.1:8080", "127.0.0.1:8080", "", 8080));
    EXPECT_TRUE(websocket_allowed("http://localhost:8080", "localhost:8080", "", 8080));
    EXPECT_FALSE(websocket_allowed("http://evil.example", "127.0.0.1:8080", "", 8080));  // another site's page
    EXPECT_FALSE(websocket_allowed("http://evil.example:8080", "evil.example:8080", "", 8080));  // DNS rebinding
    EXPECT_FALSE(websocket_allowed("", "127.0.0.1:8080", "", 8080));  // no Origin
    EXPECT_FALSE(websocket_allowed("https://127.0.0.1:8080", "127.0.0.1:8080", "", 8080));
    EXPECT_FALSE(websocket_allowed("http://", "", "t0k3n", 8080));

    EXPECT_TRUE(websocket_allowed("http://box.lan:8080", "box.lan:8080", "t0k3n", 8080));
    EXPECT_FALSE(websocket_allowed("http://evil.example", "box.lan:8080", "t0k3n", 8080));
}

}  // namespace
}  // namespace simulator::server
