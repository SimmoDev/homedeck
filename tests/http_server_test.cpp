#include "platform/host/http_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace {

// A minimal real HTTP/1.1 client over a raw socket - proving
// HostHttpServer actually accepts and answers a real TCP connection, not
// just that its internal dispatch logic runs. No new dependency: plain
// POSIX sockets, matching how this project already avoids adding a
// library where the standard one suffices.
std::string HttpGet(uint16_t port, const std::string& path, const std::string& extra_request_headers = "") {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "";
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return "";
    }

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n" + extra_request_headers +
                           "Connection: close\r\n\r\n";
    send(sock, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<size_t>(received));
    }
    close(sock);
    return response;
}

// Same real-socket approach as HttpGet, for a POST with a body large
// enough (multiple MB) that civetweb's mg_read() cannot return it in a
// single call - the scenario the body-read loop in Dispatch() exists
// for (see src/platform/host/http_server.cpp).
std::string HttpPost(uint16_t port, const std::string& path, const std::string& body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "";
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return "";
    }

    std::string request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\n" +
                           "Content-Length: " + std::to_string(body.size()) + "\r\n" + "Connection: close\r\n\r\n" +
                           body;

    size_t total_sent = 0;
    while (total_sent < request.size()) {
        ssize_t sent = send(sock, request.data() + total_sent, request.size() - total_sent, 0);
        if (sent <= 0) {
            break;
        }
        total_sent += static_cast<size_t>(sent);
    }

    std::string response;
    char buffer[4096];
    ssize_t received;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<size_t>(received));
    }
    close(sock);
    return response;
}

}  // namespace

TEST(HostHttpServer, RespondsToARealRequestOverARealSocket) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello",
                            [](const homedeck::HttpRequest&) {
                                return homedeck::HttpResponse{200, "text/plain", "world", {}};
                            });
    ASSERT_TRUE(server.Start(18181));

    std::string response = HttpGet(18181, "/hello");

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("world"), std::string::npos);
}

TEST(HostHttpServer, RequestCookieHeaderIsReadableAndResponseExtraHeadersAreSent) {
    homedeck::HostHttpServer server;
    std::string seen_cookie;
    server.RegisterHandler(
        homedeck::HttpMethod::kGet, "/echo-cookie",
        [&seen_cookie](const homedeck::HttpRequest& request) {
            seen_cookie = request.cookie_header.value_or("");
            homedeck::HttpResponse response{200, "text/plain", "ok", {}};
            response.extra_headers.push_back({"Set-Cookie", "session=abc123; HttpOnly"});
            return response;
        });
    ASSERT_TRUE(server.Start(18183));

    std::string response = HttpGet(18183, "/echo-cookie", "Cookie: session=xyz\r\n");

    EXPECT_EQ(seen_cookie, "session=xyz");
    EXPECT_NE(response.find("Set-Cookie: session=abc123; HttpOnly"), std::string::npos);
}

TEST(HostHttpServer, UnregisteredPathReturns404) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello",
                            [](const homedeck::HttpRequest&) {
                                return homedeck::HttpResponse{200, "text/plain", "world", {}};
                            });
    ASSERT_TRUE(server.Start(18182));

    std::string response = HttpGet(18182, "/nope");

    EXPECT_NE(response.find("404"), std::string::npos);
}

TEST(HostHttpServer, LargeRequestBodyIsReadInFull) {
    // Big enough that mg_read() cannot satisfy it in a single call -
    // exercises the read loop rather than the (already-covered) single-
    // call case.
    constexpr size_t kBodySize = 4 * 1024 * 1024;
    std::string body(kBodySize, '\0');
    for (size_t i = 0; i < kBodySize; ++i) {
        body[i] = static_cast<char>(i % 251);
    }

    homedeck::HostHttpServer server;
    size_t received_size = 0;
    bool content_matched = false;
    server.RegisterHandler(homedeck::HttpMethod::kPost, "/upload",
                            [&](const homedeck::HttpRequest& request) {
                                received_size = request.body.size();
                                content_matched = request.body == body;
                                return homedeck::HttpResponse{200, "text/plain", "ok", {}};
                            });
    ASSERT_TRUE(server.Start(18184));

    std::string response = HttpPost(18184, "/upload", body);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_EQ(received_size, kBodySize);
    EXPECT_TRUE(content_matched);
}
