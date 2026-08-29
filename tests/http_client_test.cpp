#include "platform/host/http_client.h"

#include "platform/host/http_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

// A real client/server round trip against a real local HostHttpServer
// instance - hermetic and non-flaky (no live internet dependency),
// while still proving HostHttpClient does genuine network I/O rather
// than exercising only its own internal logic. Matches this project's
// "test for real, not mocked" precedent (AdminAuthService's real
// PBKDF2 test, http_server_test.cpp's own raw-socket round trip).
//
// Servers bind an ephemeral port (Start(0)) rather than a fixed one so
// a parallel or rapidly-repeated run can't collide - see
// HostHttpServer::BoundPort().

namespace {

std::string LoopbackUrl(uint16_t port, const std::string& path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

}  // namespace

TEST(HostHttpClient, GetReturnsARealResponseFromARealServer) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello", [](const homedeck::HttpRequest&) {
        return homedeck::HttpResponse{200, "text/plain", "world", {}};
    });
    ASSERT_TRUE(server.Start(0));

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get(LoopbackUrl(server.BoundPort(), "/hello"));

    EXPECT_TRUE(response.success);
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "world");
}

TEST(HostHttpClient, UnregisteredPathReturns404) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello", [](const homedeck::HttpRequest&) {
        return homedeck::HttpResponse{200, "text/plain", "world", {}};
    });
    ASSERT_TRUE(server.Start(0));

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get(LoopbackUrl(server.BoundPort(), "/nope"));

    // A 404 is still a real, received response - success is about the
    // transport, not the HTTP-level outcome (see http_client.h).
    EXPECT_TRUE(response.success);
    EXPECT_EQ(response.status_code, 404);
}

TEST(HostHttpClient, PostSendsJsonBodyAndReturnsARealResponse) {
    homedeck::HostHttpServer server;
    std::string received_body;
    server.RegisterHandler(homedeck::HttpMethod::kPost, "/echo",
                            [&received_body](const homedeck::HttpRequest& request) {
                                received_body = request.body;
                                return homedeck::HttpResponse{200, "application/json", R"({"ok":true})", {}};
                            });
    ASSERT_TRUE(server.Start(0));

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Post(LoopbackUrl(server.BoundPort(), "/echo"), R"({"id":1})");

    EXPECT_TRUE(response.success);
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, R"({"ok":true})");
    EXPECT_EQ(received_body, R"({"id":1})");
}

// HttpRequest (platform/http_server.h) only surfaces the Cookie header
// today - HostHttpServer can't prove an arbitrary outbound header was
// sent the way PostSendsJsonBodyAndReturnsARealResponse above proves the
// body was. A raw listening socket, reading the request as plain text,
// is the only way to see it - the same "test for real, not mocked"
// reasoning this file's own top comment already follows. This guards
// against a specific regression, not incidental coverage: extra_headers
// exists because a live probe against the reference Harmony hub during
// this feature's design found its handshake endpoint rejects the request
// without an Origin header (see ADR-0029) - silently dropping this
// parameter again would resurface that exact failure.
TEST(HostHttpClient, PostSendsExtraHeaders) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);  // kernel-assigned free port
    ASSERT_EQ(bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(listen_fd, 1), 0);
    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len), 0);
    const uint16_t port = ntohs(addr.sin_port);

    std::string received_request;
    std::thread server_thread([listen_fd, &received_request] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        char buffer[4096];
        ssize_t n = read(conn_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            received_request.assign(buffer, static_cast<size_t>(n));
        }
        const char response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        write(conn_fd, response, sizeof(response) - 1);
        close(conn_fd);
    });

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response =
        client.Post(LoopbackUrl(port, "/echo"), "{}", {{"Origin", "http://example.com"}});

    server_thread.join();
    close(listen_fd);

    EXPECT_TRUE(response.success);
    EXPECT_NE(received_request.find("Origin: http://example.com"), std::string::npos);
}

TEST(HostHttpClient, UnreachableServerReturnsFailure) {
    // Bind then immediately release an ephemeral port to get a number
    // nothing is listening on - a real connection-refused case, not a
    // timeout (which would make this test slow). The gap between release
    // and connect is a theoretical TOCTOU, not a practical one on
    // loopback.
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(probe, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    ASSERT_EQ(bind(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(getsockname(probe, reinterpret_cast<sockaddr*>(&addr), &addr_len), 0);
    const uint16_t closed_port = ntohs(addr.sin_port);
    close(probe);

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get(LoopbackUrl(closed_port, "/anything"));

    EXPECT_FALSE(response.success);
    EXPECT_EQ(response.status_code, 0);
}
