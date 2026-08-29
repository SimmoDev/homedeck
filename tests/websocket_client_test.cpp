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

// Binds a kernel-assigned free loopback port (reported via *out_port) so
// parallel or rapidly-repeated runs can't collide on a fixed one.
// Returns -1 on any failure rather than a half-open fd, so the caller
// can ASSERT instead of accept()ing on a socket that never listened.
int ListenOnLoopback(uint16_t* out_port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    socklen_t addr_len = sizeof(addr);
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 1) != 0 ||
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        close(listen_fd);
        return -1;
    }
    *out_port = ntohs(addr.sin_port);
    return listen_fd;
}

}  // namespace

TEST(HostWebSocketClient, ConnectSendAndReceiveARealTextFrameRoundTrip) {
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);
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
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    ASSERT_TRUE(client.SendText(R"({"ping":true})"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    EXPECT_EQ(received_from_client, R"({"ping":true})");
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, R"({"hello":"world"})");
}

TEST(HostWebSocketClient, ReceiveTextReassemblesOneFrameDeliveredAcrossMultipleReads) {
    // A single WS frame whose payload is larger than ReadExact()'s own
    // per-call read size arrives via more than one poll()/curl_easy_recv()
    // round trip - ReceiveText()'s ReadExact() calls must keep
    // accumulating rather than returning early on the first chunk. This
    // is distinct from RFC 6455 multi-frame continuation (FIN=0 then a
    // continuation frame, covered by
    // ReceiveTextReassemblesARealMultiFrameContinuationMessage below) -
    // one large single frame, not several small ones.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);
    std::string payload = "{\"activities\":[" + std::string(20000, 'a') + "]}";

    std::jthread server_thread([listen_fd, &payload] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        SendServerTextFrame(conn_fd, payload);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, payload);
}

TEST(HostWebSocketClient, ReceiveTextReassemblesARealMultiFrameContinuationMessage) {
    // RFC 6455 multi-frame continuation: FIN=0 on the first frame (TEXT
    // opcode), then one or more FIN=0 CONTINUATION frames, then a final
    // FIN=1 CONTINUATION frame - every payload this project has observed
    // against the reference hub arrives as a single frame (ADR-0029), but
    // ReceiveText()'s own loop handles this shape too; nothing before
    // this test actually exercised it against the real backend.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);

        std::vector<unsigned char> first = BuildServerFrameHeader(/*fin=*/false, /*opcode=*/0x1, 5);
        send(conn_fd, first.data(), first.size(), MSG_NOSIGNAL);
        send(conn_fd, "hello", 5, MSG_NOSIGNAL);

        std::vector<unsigned char> middle = BuildServerFrameHeader(/*fin=*/false, /*opcode=*/0x0, 1);
        send(conn_fd, middle.data(), middle.size(), MSG_NOSIGNAL);
        send(conn_fd, " ", 1, MSG_NOSIGNAL);

        std::vector<unsigned char> last = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x0, 5);
        send(conn_fd, last.data(), last.size(), MSG_NOSIGNAL);
        send(conn_fd, "world", 5, MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, "hello world");
}

TEST(HostWebSocketClient, ReceiveTextRepliesToAPingAndKeepsWaitingForTheRealMessage) {
    // RFC 6455's PING/PONG keep-alive: a PING frame gets an in-kind PONG
    // reply (echoing its payload) and must not be surfaced to this call's
    // own caller as message content - the caller is waiting for the
    // getCurrentActivity/config response the hub actually sent, not a
    // transport-level keep-alive.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);
    std::string received_pong_payload;

    std::jthread server_thread([listen_fd, &received_pong_payload] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);

        std::string ping_payload = "keepalive";
        std::vector<unsigned char> ping_header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x9, ping_payload.size());
        send(conn_fd, ping_header.data(), ping_header.size(), MSG_NOSIGNAL);
        send(conn_fd, ping_payload.data(), ping_payload.size(), MSG_NOSIGNAL);

        unsigned char reply_header[2];
        if (read(conn_fd, reply_header, 2) == 2 && (reply_header[0] & 0x0F) == 0xA) {
            bool masked = (reply_header[1] & 0x80) != 0;
            uint64_t len = reply_header[1] & 0x7F;
            unsigned char mask[4] = {};
            if (masked) read(conn_fd, mask, 4);
            std::string payload(len, '\0');
            size_t got = 0;
            while (got < len) {
                ssize_t n = read(conn_fd, &payload[got], len - got);
                if (n <= 0) break;
                got += static_cast<size_t>(n);
            }
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
                }
            }
            received_pong_payload = payload;
        }

        SendServerTextFrame(conn_fd, R"({"hello":"world"})");
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    EXPECT_EQ(received_pong_payload, "keepalive");
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(*response, R"({"hello":"world"})");
}

TEST(HostWebSocketClient, ReceiveTextRejectsAMessageOverTheSizeCap) {
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

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
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(5000);

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(response.has_value());
}

TEST(HostWebSocketClient, ReceiveTextEchoesACloseFrameBack) {
    // RFC 6455's closing handshake: the endpoint that receives a CLOSE
    // frame is expected to send one back before the TCP connection
    // actually closes.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);
    uint8_t received_opcode = 0xFF;  // sentinel - never a valid opcode nibble on its own

    std::jthread server_thread([listen_fd, &received_opcode] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        std::vector<unsigned char> close_header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x8, 0);
        send(conn_fd, close_header.data(), close_header.size(), MSG_NOSIGNAL);

        unsigned char reply_header[2];
        if (read(conn_fd, reply_header, 2) == 2) {
            received_opcode = reply_header[0] & 0x0F;
        }
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(2000);

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(response.has_value());
    EXPECT_EQ(received_opcode, 0x8);
}

TEST(HostWebSocketClient, SendTextFailsAfterReceivingACloseFrame) {
    // Regression test: ReceiveText() observing a peer CLOSE frame used to
    // leave curl_ untouched, so a SendText() call landing right afterward
    // (e.g. HarmonyConnection::DrainStaleMessages()'s own 0ms poll running
    // just before a send) could attempt - and, since the underlying TCP
    // socket often stays briefly writable past a WS-level close, even
    // succeed - a write on a connection the peer has already ended,
    // instead of failing outright the way a caller already treats a dead
    // connection.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        std::vector<unsigned char> close_header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x8, 0);
        send(conn_fd, close_header.data(), close_header.size(), MSG_NOSIGNAL);
        // Keeps the TCP socket itself open a little past the WS-level
        // close, so a regressed SendText() below would have a real chance
        // to succeed at the TCP layer - closing immediately would fail
        // that write for an unrelated reason (ECONNRESET) even without
        // this fix, making the test pass for the wrong reason.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    EXPECT_FALSE(client.ReceiveText(2000).has_value());
    EXPECT_FALSE(client.SendText(R"({"probe":"after-close"})"));

    server_thread.join();
    close(listen_fd);
}

TEST(HostWebSocketClient, ReceiveTextRejectsABinaryFrame) {
    // RFC 6455 requires failing the connection on a reserved/unexpected
    // opcode (0x2 binary here) rather than silently treating it as
    // message content - this transport has no authentication at all
    // (ADR-0029), so a hostile/misbehaving LAN peer sending one is a
    // real possibility.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        std::string payload = "not text";
        std::vector<unsigned char> header = BuildServerFrameHeader(/*fin=*/true, /*opcode=*/0x2, payload.size());
        send(conn_fd, header.data(), header.size(), MSG_NOSIGNAL);
        send(conn_fd, payload.data(), payload.size(), MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));
    std::optional<std::string> response = client.ReceiveText(2000);

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
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

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
    bool connected = client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/");

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(connected);
}

TEST(HostWebSocketClient, ConnectFailsWhenSecWebSocketAcceptIsMissingEntirely) {
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

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
    bool connected = client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/");

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(connected);
}

TEST(HostWebSocketClient, ConnectFailsWhenUpgradeHeaderIsMissingOrWrong) {
    // RFC 6455 4.2.2 requires the client to check the response's Upgrade
    // header too, not just Sec-WebSocket-Accept - a correct Accept key
    // alone (this server computes one for real) doesn't by itself prove
    // the response is shaped as a WebSocket upgrade.
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        std::string request = ReadHttpRequest(conn_fd);
        std::string client_key = ExtractHeaderValue(request, "Sec-WebSocket-Key");
        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Accept: " +
                                ComputeAcceptKey(client_key) + "\r\n\r\n";
        send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    bool connected = client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/");

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(connected);
}

TEST(HostWebSocketClient, ConnectFailsWhenConnectionHeaderIsMissingOrWrong) {
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        std::string request = ReadHttpRequest(conn_fd);
        std::string client_key = ExtractHeaderValue(request, "Sec-WebSocket-Key");
        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: websocket\r\n"
                                "Sec-WebSocket-Accept: " +
                                ComputeAcceptKey(client_key) + "\r\n\r\n";
        send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    bool connected = client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/");

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
    uint16_t port = 0;
    int listen_fd = ListenOnLoopback(&port);
    ASSERT_GE(listen_fd, 0);

    std::jthread server_thread([listen_fd] {
        int conn_fd = accept(listen_fd, nullptr, nullptr);
        if (conn_fd < 0) return;
        PerformServerHandshake(conn_fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        close(conn_fd);
    });

    homedeck::HostWebSocketClient client;
    ASSERT_TRUE(client.Connect("ws://127.0.0.1:" + std::to_string(port) + "/"));

    auto start = std::chrono::steady_clock::now();
    std::optional<std::string> response = client.ReceiveText(0);
    auto elapsed = std::chrono::steady_clock::now() - start;

    server_thread.join();
    close(listen_fd);

    EXPECT_FALSE(response.has_value());
    EXPECT_LT(elapsed, std::chrono::milliseconds(200));
}
