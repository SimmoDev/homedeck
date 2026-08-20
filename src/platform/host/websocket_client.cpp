#include "platform/host/websocket_client.h"

#include <curl/curl.h>

#include <poll.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace homedeck {

namespace {

// See platform/host/http_client.cpp's identical comment - curl_global_init()
// must run exactly once, before any curl handle is used, regardless of how
// many HostHttpClient/HostWebSocketClient instances get constructed.
void EnsureGlobalInit() {
    static int unused = (curl_global_init(CURL_GLOBAL_DEFAULT), 0);
    (void)unused;
}

// Bounds how long the connect handshake below can block - libcurl's own
// default connect timeout (300s) would otherwise leave the connection-loop
// thread (and Task::~Task() during shutdown) unresponsive for minutes
// against an unreachable/black-hole host. Same reasoning as
// platform/host/http_client.cpp's kTimeoutSeconds. Also used to bound the
// hand-built Upgrade request/response exchange in Connect() below, and
// SendText()'s own write.
constexpr int kConnectTimeoutMs = 10000;

// A malformed/adversarial handshake response (this transport has no
// authentication at all, see ADR-0029) must not be read without bound -
// matches the order of magnitude of esp_http_client's own header-size
// assumptions elsewhere in this project (hardware.md's Wi-Fi setup form
// section names 4096 for a comparable reason).
constexpr size_t kMaxHandshakeResponseBytes = 8192;

// See this file's own header comment for why ws:// itself can't be handed
// to curl on every system libcurl build.
std::string ToHttpScheme(const std::string& ws_url) {
    if (ws_url.rfind("wss://", 0) == 0) return "https://" + ws_url.substr(6);
    if (ws_url.rfind("ws://", 0) == 0) return "http://" + ws_url.substr(5);
    return ws_url;
}

// Splits a ws(s):// URL into its Host header value ("host:port") and
// request-target ("/path?query", defaulting to "/"). Not a general-purpose
// URL parser - matches exactly how this project's own callers build these
// URLs (HandshakeUrl()/WebSocketUrl() in core/harmony_connection.cpp,
// "ws://" + hub_host + ":8088/..." - hub_host is already validated by
// IsValidHubHost() to exclude '/'/'#'/'?'/'@' and bare unbracketed colons,
// so a plain split on the first '/' after the scheme is unambiguous).
struct ParsedUrl {
    std::string host_header;
    std::string request_target;
};

ParsedUrl ParseWsUrl(const std::string& url) {
    ParsedUrl result;
    size_t scheme_end = url.find("://");
    size_t host_start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    if (path_start == std::string::npos) {
        result.host_header = url.substr(host_start);
        result.request_target = "/";
    } else {
        result.host_header = url.substr(host_start, path_start - host_start);
        result.request_target = url.substr(path_start);
    }
    return result;
}

void FillRandomBytes(unsigned char* buf, size_t len) {
    // Not a security-sensitive value - only ever used for the
    // Sec-WebSocket-Key handshake nonce (a protocol-conformance
    // requirement, not an auth mechanism) and per-frame client masks;
    // this project's own hub protocol has no authentication at all
    // (ADR-0029). std::random_device over mbedtls avoids pulling a
    // dependency into this platform-layer target for a non-cryptographic
    // need mbedcrypto is only otherwise linked into homedeck_core for.
    static std::random_device rd;
    for (size_t i = 0; i < len; ++i) buf[i] = static_cast<unsigned char>(rd());
}

// RFC 4648 base64 - only consumer is the Sec-WebSocket-Key header below.
std::string Base64Encode(const unsigned char* data, size_t len) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6) & 0x3F];
        out += kTable[n & 0x3F];
    }
    size_t remaining = len - i;
    if (remaining == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += "==";
    } else if (remaining == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += kTable[(n >> 18) & 0x3F];
        out += kTable[(n >> 12) & 0x3F];
        out += kTable[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

std::string GenerateWebSocketKey() {
    unsigned char bytes[16];
    FillRandomBytes(bytes, sizeof(bytes));
    return Base64Encode(bytes, sizeof(bytes));
}

// Self-contained SHA-1 (RFC 3174) - only consumer is ComputeAcceptKey()
// below, the RFC 6455 handshake's Sec-WebSocket-Accept verification step.
// Hand-rolled rather than pulling in mbedtls (already vendored for
// AdminAuthService's password hashing, src/core/admin_auth_service.cpp)
// for the same reason Base64Encode() above is hand-rolled instead of
// using mbedtls's own base64 codec: mbedcrypto is only linked into the
// homedeck_core target, not homedeck_platform_host (this file's own
// target - see src/CMakeLists.txt), and this is a small, stable,
// well-specified primitive, not something worth restructuring that
// dependency graph for. Not a security-sensitive use (this project's
// own hub protocol has no authentication at all, ADR-0029) - only
// confirms the handshake response actually came from something that
// understood the request as a WebSocket upgrade, not a general SHA-1
// consumer.
std::array<unsigned char, 20> Sha1(const std::string& input) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    std::vector<unsigned char> msg(input.begin(), input.end());
    const uint64_t bit_length = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<unsigned char>((bit_length >> (8 * i)) & 0xFF));

    auto left_rotate = [](uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); };

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) | (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) | static_cast<uint32_t>(msg[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = left_rotate(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<unsigned char, 20> digest;
    uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<unsigned char>((words[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<unsigned char>((words[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<unsigned char>((words[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<unsigned char>(words[i] & 0xFF);
    }
    return digest;
}

// RFC 6455 section 1.3's handshake verification: base64(SHA1(key +
// this fixed GUID)) - confirms the response came from something that
// actually understood the request as a WebSocket upgrade, not just
// something that happened to answer "101" (a misconfigured device, a
// captive-portal-style proxy, a different service that took over the
// hub's IP after a DHCP lease change).
std::string ComputeAcceptKey(const std::string& client_key) {
    static constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::array<unsigned char, 20> digest = Sha1(client_key + kWebSocketGuid);
    return Base64Encode(digest.data(), digest.size());
}

// Case-insensitive header lookup on the raw \r\n-delimited handshake
// response headers (status line included, harmlessly - it never matches
// a colon-prefixed header name). Not a general-purpose HTTP header
// parser - matches ParseWsUrl()'s own "just enough for this project's
// own request/response shapes" precedent.
std::optional<std::string> FindHeaderValue(const std::string& headers, const std::string& name) {
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t line_end = headers.find("\r\n", pos);
        if (line_end == std::string::npos) break;
        std::string line = headers.substr(pos, line_end - pos);
        pos = line_end + 2;

        size_t colon = line.find(':');
        if (colon == std::string::npos || colon != name.size()) continue;
        bool matches = std::equal(name.begin(), name.end(), line.begin(),
                                   [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        if (!matches) continue;

        size_t value_start = colon + 1;
        while (value_start < line.size() && line[value_start] == ' ') ++value_start;
        return line.substr(value_start);
    }
    return std::nullopt;
}

int RemainingMs(std::chrono::steady_clock::time_point deadline) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;
}

// Reads exactly `len` bytes into `buf` via curl_easy_recv() (not a raw
// system recv() on the extracted socket) - transparently correct whether
// or not the underlying connection is TLS, matching curl's own documented
// CONNECT_ONLY use case ("let the application drive the transfer using
// curl_easy_send()/curl_easy_recv()"). poll() on the raw fd is still valid
// for readiness-waiting regardless: it operates below the TLS layer,
// which needs an actually-readable socket to make its own progress on
// either side. Used for both the frame header/payload reads in
// ReceiveText() and the handshake response read in Connect() below, since
// any of them can arrive split across more than one read. False on
// timeout, error, or a clean close before `len` bytes arrive.
bool ReadExact(CURL* curl, curl_socket_t fd, unsigned char* buf, size_t len, std::chrono::steady_clock::time_point deadline) {
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int poll_result = poll(&pfd, 1, RemainingMs(deadline));
        if (poll_result <= 0 || !(pfd.revents & POLLIN)) return false;
        size_t n = 0;
        CURLcode rc = curl_easy_recv(curl, buf + got, len - got, &n);
        if (rc == CURLE_AGAIN) continue;  // spurious wakeup - keep polling within the deadline
        if (rc != CURLE_OK || n == 0) return false;
        got += n;
    }
    return true;
}

// Sends exactly `len` bytes via curl_easy_send() - same TLS-transparency
// reasoning as ReadExact() above.
bool WriteExact(CURL* curl, curl_socket_t fd, const unsigned char* buf, size_t len, std::chrono::steady_clock::time_point deadline) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_result = poll(&pfd, 1, RemainingMs(deadline));
        if (poll_result <= 0 || !(pfd.revents & POLLOUT)) return false;
        size_t n = 0;
        CURLcode rc = curl_easy_send(curl, buf + sent, len - sent, &n);
        if (rc == CURLE_AGAIN) continue;
        if (rc != CURLE_OK || n == 0) return false;
        sent += n;
    }
    return true;
}

// Builds one complete, masked (client->server, per RFC 6455) frame - used
// for both SendText()'s text frames and ReceiveText()'s own PONG replies.
std::vector<unsigned char> BuildMaskedFrame(uint8_t opcode, const std::string& payload) {
    std::vector<unsigned char> frame;
    frame.push_back(static_cast<unsigned char>(0x80 | opcode));  // FIN + opcode
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<unsigned char>(0x80 | len));  // MASK bit + length
    } else if (len <= 0xFFFF) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(len & 0xFF));
    } else {
        frame.push_back(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame.push_back(static_cast<unsigned char>((len >> (8 * i)) & 0xFF));
    }
    unsigned char mask[4];
    FillRandomBytes(mask, sizeof(mask));
    frame.insert(frame.end(), mask, mask + 4);
    size_t header_len = frame.size();
    frame.resize(header_len + len);
    for (size_t i = 0; i < len; ++i) {
        frame[header_len + i] = static_cast<unsigned char>(payload[i]) ^ mask[i % 4];
    }
    return frame;
}

}  // namespace

HostWebSocketClient::~HostWebSocketClient() { Close(); }

bool HostWebSocketClient::Connect(const std::string& url) {
    EnsureGlobalInit();
    Close();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ToHttpScheme(url).c_str());
    // 1L, not 2L: stop right after the TCP(+TLS) connect, before curl
    // sends any HTTP request of its own - see this file's own header
    // comment for why curl's generic HTTP engine can't be trusted with
    // the Upgrade request/response here (it has no way to know a 101
    // response is intentionally bodyless without going through the ws://
    // scheme handler this build lacks, and instead waits for a body or
    // EOF that a real WS server - correctly keeping the connection open
    // - never sends). The handshake is hand-built below instead.
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kConnectTimeoutMs);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_socket_t sockfd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd) != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        curl_easy_cleanup(curl);
        return false;
    }

    ParsedUrl parsed = ParseWsUrl(url);
    std::string key = GenerateWebSocketKey();
    std::string request = "GET " + parsed.request_target +
                           " HTTP/1.1\r\n"
                           "Host: " +
                           parsed.host_header +
                           "\r\n"
                           "Connection: Upgrade\r\n"
                           "Upgrade: websocket\r\n"
                           "Sec-WebSocket-Version: 13\r\n"
                           "Sec-WebSocket-Key: " +
                           key + "\r\n\r\n";

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    if (!WriteExact(curl, sockfd, reinterpret_cast<const unsigned char*>(request.data()), request.size(), deadline)) {
        curl_easy_cleanup(curl);
        return false;
    }

    std::string response;
    unsigned char byte;
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() >= kMaxHandshakeResponseBytes) {
            curl_easy_cleanup(curl);
            return false;
        }
        if (!ReadExact(curl, sockfd, &byte, 1, deadline)) {
            curl_easy_cleanup(curl);
            return false;
        }
        response += static_cast<char>(byte);
    }
    size_t status_line_end = response.find("\r\n");
    std::string status_line = response.substr(0, status_line_end);
    size_t first_space = status_line.find(' ');
    if (first_space == std::string::npos || status_line.compare(first_space + 1, 3, "101") != 0) {
        curl_easy_cleanup(curl);
        return false;
    }

    // A bare "101" isn't enough on its own - RFC 6455's own handshake
    // verification step confirms the far end actually understood the
    // request as a WebSocket upgrade (see ComputeAcceptKey()'s own
    // comment), not just something that happens to answer 101.
    std::optional<std::string> accept = FindHeaderValue(response, "Sec-WebSocket-Accept");
    if (!accept.has_value() || *accept != ComputeAcceptKey(key)) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_ = curl;
    return true;
}

bool HostWebSocketClient::SendText(const std::string& text) {
    if (curl_ == nullptr) {
        return false;
    }
    CURL* curl = static_cast<CURL*>(curl_);

    // Bounded the same way ReceiveText() bounds its own reads - without
    // this, a hub that stops draining its receive buffer (hung process,
    // not a clean close) would block this thread indefinitely, and with
    // it Task::~Task()'s own "stops and joins promptly" contract, since
    // nothing checks stop_token mid-call (see HarmonyConnection's own
    // comment on why).
    curl_socket_t sockfd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd) != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        return false;
    }

    std::vector<unsigned char> frame = BuildMaskedFrame(0x1, text);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectTimeoutMs);
    return WriteExact(curl, sockfd, frame.data(), frame.size(), deadline);
}

std::optional<std::string> HostWebSocketClient::ReceiveText(int timeout_ms) {
    if (curl_ == nullptr) {
        return std::nullopt;
    }
    CURL* curl = static_cast<CURL*>(curl_);

    curl_socket_t sockfd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd) != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        return std::nullopt;
    }

    std::string accumulated;
    // RemainingMs() (used by ReadExact()'s own poll() calls) already
    // clamps a non-positive remaining duration to 0 rather than skipping
    // the poll call entirely - timeout_ms=0 (DrainStaleMessages()'s own
    // non-blocking poll) must still reach poll() at least once rather
    // than returning std::nullopt unconditionally.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        unsigned char header[2];
        if (!ReadExact(curl, sockfd, header, sizeof(header), deadline)) return std::nullopt;

        bool fin = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        // RFC 6455 requires failing the connection on receipt of a
        // reserved (0x3-0x7, 0xB-0xF) or otherwise-unexpected (0x2,
        // binary) opcode - bail before reading anything else about this
        // frame (its length may itself be adversarial) rather than
        // falling through to the TEXT/CONTINUATION accumulation path
        // below, which would otherwise silently treat it as message
        // content. This transport has no authentication at all
        // (ADR-0029), so a hostile/misbehaving LAN peer sending one is a
        // real possibility, not just a spec technicality.
        if (opcode != 0x0 && opcode != 0x1 && opcode != 0x8 && opcode != 0x9 && opcode != 0xA) {
            return std::nullopt;
        }
        // Real WS servers never mask frames sent to a client (RFC 6455),
        // but this transport has no authentication at all (ADR-0029) - a
        // malformed or hostile peer setting the mask bit anyway is still
        // handled correctly here rather than silently corrupting the
        // payload.
        bool masked = (header[1] & 0x80) != 0;
        uint64_t len = header[1] & 0x7F;
        if (len == 126) {
            unsigned char ext[2];
            if (!ReadExact(curl, sockfd, ext, sizeof(ext), deadline)) return std::nullopt;
            len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            unsigned char ext[8];
            if (!ReadExact(curl, sockfd, ext, sizeof(ext), deadline)) return std::nullopt;
            len = 0;
            for (unsigned char b : ext) len = (len << 8) | b;
        }
        unsigned char mask[4] = {};
        if (masked) {
            if (!ReadExact(curl, sockfd, mask, sizeof(mask), deadline)) return std::nullopt;
        }

        if (opcode == 0x8) {  // CLOSE - echo a bare close frame back per
                               // RFC 6455's own closing handshake,
                               // best-effort same as the PONG reply below
                               // (a failed write here only affects how
                               // cleanly this side's TCP connection
                               // closes, not correctness - Close() tears
                               // down the handle regardless, and the
                               // caller already treats std::nullopt as a
                               // dead connection).
            std::vector<unsigned char> close_frame = BuildMaskedFrame(0x8, "");
            WriteExact(curl, sockfd, close_frame.data(), close_frame.size(), deadline);
            return std::nullopt;
        }

        // See kMaxWebSocketMessageBytes's own comment for why. Checked
        // against `len` alone first (cheap, no overflow risk even for an
        // adversarial 64-bit extended length) before the cumulative
        // check, which only matters once a multi-frame message is
        // actually in progress.
        if (len > kMaxWebSocketMessageBytes || accumulated.size() + len > kMaxWebSocketMessageBytes) {
            return std::nullopt;
        }

        std::string payload(len, '\0');
        if (len > 0) {
            if (!ReadExact(curl, sockfd, reinterpret_cast<unsigned char*>(payload.data()), len, deadline)) return std::nullopt;
            if (masked) {
                for (size_t i = 0; i < payload.size(); ++i) {
                    payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
                }
            }
        }

        if (opcode == 0x9) {  // PING - reply in kind (echoing the payload,
                               // per RFC 6455) and keep waiting; not a
                               // reply to anything this class's own
                               // caller sent, so it must not be treated
                               // as accumulated message content.
            std::vector<unsigned char> pong = BuildMaskedFrame(0xA, payload);
            // Best-effort - a lost/failed PONG only risks the hub's own
            // keep-alive timeout closing the connection, which the
            // connection loop already recovers from via reconnect; a
            // send failure here doesn't need to propagate to this call's
            // own caller.
            WriteExact(curl, sockfd, pong.data(), pong.size(), deadline);
            continue;
        }
        if (opcode == 0xA) {  // PONG - nothing sent an unsolicited ping expecting one; ignore.
            continue;
        }

        // TEXT (0x1) or CONTINUATION (0x0).
        accumulated += payload;
        if (fin) {
            return accumulated;
        }
        // Not the final frame of this message - loop to collect the rest
        // within the same overall deadline. This correctly reassembles a
        // real RFC 6455 multi-frame message (FIN=0 then a continuation
        // frame), not just one large frame split across multiple reads -
        // every payload this project has observed arrives as a single
        // frame regardless (see ADR-0029's Consequences), but nothing
        // here depends on that being permanent.
    }
}

void HostWebSocketClient::Close() {
    if (curl_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
}

}  // namespace homedeck
