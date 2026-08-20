#include "platform/host/websocket_client.h"

#include <gtest/gtest.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// A real client/server WebSocket round trip against a hand-rolled RFC 6455
// server on a raw socket - the same "test for real, not mocked" precedent
// http_client_test.cpp's PostSendsExtraHeaders test already sets, extended
// to this transport. harmony_connection_test.cpp exercises HarmonyConnection
// against a scriptable WebSocketClient fake; nothing exercised
// HostWebSocketClient's actual libcurl-backed implementation until now -
// three separate bugs in it (a ReceiveText(0) deadline race, an unbounded
// SendText(), an unguarded closed_ write) were all found by hand across
// prior review passes, none by a test.
//
// civetweb (already vendored for HostHttpServer) has WebSocket support
// available but disabled at this project's build (CIVETWEB_ENABLE_WEBSOCKETS
// is off), so this writes just enough of the protocol directly rather than
// enabling a whole server feature for one test file.

namespace {

std::string ComputeAcceptKey(const std::string& client_key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = client_key + kGuid;
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), digest);
    unsigned char encoded[64];
    size_t out_len = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len, digest, sizeof(digest));
    return std::string(reinterpret_cast<char*>(encoded), out_len);
}

std::string ExtractHeaderValue(const std::string& request, const std::string& header_name) {
    size_t pos = request.find(header_name + ": ");
    if (pos == std::string::npos) return "";
    pos += header_name.size() + 2;
    size_t end = request.find("\r\n", pos);
    return request.substr(pos, end - pos);
}

std::string ReadHttpRequest(int fd) {
    std::string data;
    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    return data;
}

// Completes the opening handshake: reads the client's upgrade request and
// replies with the matching Sec-WebSocket-Accept.
void PerformServerHandshake(int fd) {
    std::string request = ReadHttpRequest(fd);
    std::string client_key = ExtractHeaderValue(request, "Sec-WebSocket-Key");
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: " +
                            ComputeAcceptKey(client_key) + "\r\n\r\n";
    send(fd, response.data(), response.size(), MSG_NOSIGNAL);
}

// Reads one client->server frame (RFC 6455 requires client frames masked)
// and returns its unmasked payload. Single-frame messages only - enough
// for what these tests send.
std::string ReadClientTextFrame(int fd) {
    unsigned char header[2];
    if (read(fd, header, 2) != 2) return "";
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (read(fd, ext, 2) != 2) return "";
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (read(fd, ext, 8) != 8) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    unsigned char mask[4] = {};
    if (masked && read(fd, mask, 4) != 4) return "";
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, &payload[got], len - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
    }
    return payload;
}

std::vector<unsigned char> BuildServerFrameHeader(bool fin, uint8_t opcode, size_t payload_size) {
    std::vector<unsigned char> header;
    header.push_back(static_cast<unsigned char>((fin ? 0x80 : 0x00) | opcode));
    if (payload_size < 126) {
        header.push_back(static_cast<unsigned char>(payload_size));
    } else if (payload_size <= 0xFFFF) {
        header.push_back(126);
        header.push_back(static_cast<unsigned char>((payload_size >> 8) & 0xFF));
        header.push_back(static_cast<unsigned char>(payload_size & 0xFF));
    } else {
        header.push_back(127);
        for (int i = 7; i >= 0; --i) {
            header.push_back(static_cast<unsigned char>((payload_size >> (8 * i)) & 0xFF));
        }
    }
    return header;
}

// Sends one small, complete server->client text frame (never masked, per
// RFC 6455). A plain blocking write is fine - every caller sends at most a
// few hundred bytes.
void SendServerTextFrame(int fd, const std::string& payload) {
    std::vector<unsigned char> header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x1, payload.size());
    send(fd, header.data(), header.size(), MSG_NOSIGNAL);
    send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
}

// Sends a large payload best-effort, never blocking past deadline_ms total -
// the client under test is expected to stop draining partway through (once
// it detects the payload exceeds kMaxWebSocketMessageBytes), so a plain
// blocking write() here could otherwise hang this thread forever once the
// socket's send buffer fills.
void SendServerLargeFrameBestEffort(int fd, const std::string& payload, int deadline_ms) {
    std::vector<unsigned char> header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x1, payload.size());
    send(fd, header.data(), header.size(), MSG_NOSIGNAL);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    size_t sent = 0;
    while (sent < payload.size() && std::chrono::steady_clock::now() < deadline) {
        ssize_t n = send(fd, payload.data() + sent, payload.size() - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = {};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            poll(&pfd, 1, 50);
            continue;
        }
        break;
    }
}

int ListenOnLoopback(int port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listen_fd, 1);
    return listen_fd;
}

}  // namespace

TEST(HostWebSocketClient, ConnectSendAndReceiveARealTextFrameRoundTrip) {
    int listen_fd = ListenOnLoopback(18300);
    std::string received_from_client;

    std::jthread server_thread([listen_fd, &received_from_client] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        received_from_client = ReadClientTextFrame(conn_fd);
        SendServerTextFrame(conn_fd, R"({"hello":"world"})");
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:18300/"));
    ASSERT_TRUE(client.SendText(R"({"ping":true})"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    EXPECT_EQ(received_from_client, R"({"ping":true})");
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, R"({"hello":"world"})");
}

TEST(HostWebSocketClient, ReceiveTextReassemblesOneFrameDeliveredAcrossMultipleReads) {
    // A single WS frame whose payload is larger than ReceiveText()'s own
    // internal read buffer (8192 bytes) arrives via more than one
    // curl_ws_recv() call - curl reports this via a nonzero `bytesleft`
    // until the frame's last chunk, which ReceiveText()'s loop must keep
    // accumulating rather than returning early on the first chunk. This
    // is distinct from RFC 6455 multi-frame continuation (FIN=0 then a
    // continuation frame), which this class's own header comment
    // documents as a real, deliberate limitation it doesn't yet handle -
    // not exercised here since every payload this project has observed
    // (ADR-0029) arrives as one frame.
    int listen_fd = ListenOnLoopback(18301);
    std::string payload = "{\"activities\":[" + std::string(20000, 'a') + "]}";

    std::jthread server_thread([listen_fd, &payload] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        SendServerTextFrame(conn_fd, payload);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:18301/"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, payload);
}

TEST(HostWebSocketClient, ReceiveTextRejectsAMessageOverTheSizeCap) {
    int listen_fd = ListenOnLoopback(18302);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        // One byte over homedeck::kMaxWebSocketMessageBytes - a rogue/
        // buggy LAN device on this unauthenticated transport (ADR-0029)
        // must not be able to grow the client's accumulated buffer
        // without bound.
        std::string oversized(homedeck::kMaxWebSocketMessageBytes + 1, 'x');
        SendServerLargeFrameBestEffort(conn_fd, oversized, /*deadline_ms=*/3000);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:18302/"));
    std::optional<std::string> response = client.ReceiveText(5000);

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(response.has_value());
}

TEST(HostWebSocketClient, ConnectFailsOnAWrongSecWebSocketAcceptHeader) {
    // Regression test for Connect() previously accepting any "101" status
    // line with no check that Sec-WebSocket-Accept actually matches the
    // Sec-WebSocket-Key this client sent - RFC 6455's own handshake
    // verification step, which confirms the far end understood the
    // request as a WebSocket upgrade rather than just happening to answer
    // 101 (a misconfigured device, a captive-portal-style proxy, a
    // different service that took over the hub's IP after a DHCP lease
    // change).
    int listen_fd = ListenOnLoopback(18304);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        ReadHttpRequest(conn_fd);  // discard the client_key - deliberately not used below
        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: not-the-right-key\r\n\r\n";
        send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    bool connected = client.Connect("ws://127.0.0.1:18304/");

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(connected);
}

TEST(HostWebSocketClient, ConnectFailsWhenSecWebSocketAcceptIsMissingEntirely) {
    int listen_fd = ListenOnLoopback(18305);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        ReadHttpRequest(conn_fd);
        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n\r\n";
        send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    bool connected = client.Connect("ws://127.0.0.1:18305/");

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(connected);
}

TEST(HostWebSocketClient, ReceiveTextZeroDoesNotBlockWhenNothingIsPending) {
    // Regression test for the ReceiveText(0) deadline race that made
    // HarmonyConnection::DrainStaleMessages()'s own zero-timeout poll a
    // silent no-op on this backend: with nothing pending, ReceiveText(0)
    // must return promptly rather than blocking for the connection's
    // full lifetime.
    int listen_fd = ListenOnLoopback(18303);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:18303/"));

    auto start = std::chrono::steady_clock::now();
    std::optional<std::string> response = client.ReceiveText(0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(response.has_value());
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));
}
