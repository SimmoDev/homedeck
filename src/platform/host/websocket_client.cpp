#include "platform/host/websocket_client.h"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <poll.h>

#include <chrono>

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
// platform/host/http_client.cpp's kTimeoutSeconds.
constexpr long kConnectTimeoutSeconds = 10;

// Bounds how long SendText() below can block waiting for the socket to
// become writable - matches FirmwareWebSocketClient's own kSendTimeoutMs.
// CURLOPT_TIMEOUT (used for the connect handshake above) has no effect
// here: it only governs curl_easy_perform(), and this transport's sends
// happen directly against the raw socket CONNECT_ONLY left open, via
// curl_ws_send(), which has no timeout option of its own.
constexpr int kSendTimeoutMs = 10000;

}  // namespace

HostWebSocketClient::~HostWebSocketClient() { Close(); }

bool HostWebSocketClient::Connect(const std::string& url) {
    EnsureGlobalInit();
    Close();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // 2L: perform the WS protocol handshake during curl_easy_perform(),
    // then return with the connection left open for curl_ws_recv/
    // curl_ws_send below - see https://curl.se/libcurl/c/libcurl-ws.html.
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kConnectTimeoutSeconds);

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
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

    // Bounded the same way ReceiveText() bounds its own read - without
    // this, a hub that stops draining its receive buffer (hung process,
    // not a clean close) would block this thread indefinitely, and with
    // it Task::~Task()'s own "stops and joins promptly" contract, since
    // nothing checks stop_token mid-call (see HarmonyConnection's own
    // comment on why).
    curl_socket_t sockfd = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd) != CURLE_OK || sockfd == CURL_SOCKET_BAD) {
        return false;
    }
    struct pollfd pfd = {};
    pfd.fd = sockfd;
    pfd.events = POLLOUT;
    int poll_result = poll(&pfd, 1, kSendTimeoutMs);
    if (poll_result <= 0 || !(pfd.revents & POLLOUT)) {
        return false;
    }

    size_t sent = 0;
    CURLcode result = curl_ws_send(curl, text.data(), text.size(), &sent, 0, CURLWS_TEXT);
    return result == CURLE_OK && sent == text.size();
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
    char buffer[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        // Clamped to 0 (poll()'s own "return immediately" value), not
        // returned early on a non-positive remainder - timeout_ms=0
        // (DrainStaleMessages()'s own non-blocking poll, see its own
        // comment) must still reach poll() at least once. Computing
        // `deadline` above and checking `remaining` here are two
        // separate steady_clock reads; the wall-clock time between them
        // is enough on its own to make remaining <= 0 even for
        // timeout_ms=0, which used to skip poll() entirely and return
        // std::nullopt unconditionally - a silent no-op matching
        // FirmwareWebSocketClient's own 0ms `wait_for()` in name only,
        // not in behavior.
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        int poll_timeout_ms = remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;

        struct pollfd pfd = {};
        pfd.fd = sockfd;
        pfd.events = POLLIN;
        int poll_result = poll(&pfd, 1, poll_timeout_ms);
        if (poll_result <= 0 || !(pfd.revents & POLLIN)) {
            return std::nullopt;  // timeout or error
        }

        size_t received = 0;
        const struct curl_ws_frame* meta = nullptr;
        CURLcode result = curl_ws_recv(curl, buffer, sizeof(buffer), &received, &meta);
        if (result == CURLE_AGAIN) {
            continue;  // spurious wakeup - keep polling within the deadline
        }
        if (result != CURLE_OK || meta == nullptr) {
            return std::nullopt;
        }
        if (meta->flags & CURLWS_CLOSE) {
            return std::nullopt;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) {
            // libcurl auto-PONGs a received PING itself (default behavior,
            // CURLWS_NOAUTOPONG not set below) but still delivers the frame
            // here - not a reply to anything HostWebSocketClient's own
            // caller sent, so it must not be treated as accumulated message
            // content the way FirmwareWebSocketClient's own opcode filter
            // avoids the same mistake for esp_websocket_client's PING/PONG
            // events (see its own comment).
            continue;
        }
        // See kMaxWebSocketMessageBytes's own comment for why. Checked
        // before appending, not after - accumulated must never actually
        // exceed the bound even transiently.
        if (accumulated.size() + received > kMaxWebSocketMessageBytes) {
            return std::nullopt;
        }
        accumulated.append(buffer, received);
        if (meta->bytesleft == 0) {
            // bytesleft tracks the current WS frame, not a WS-level
            // continuation (FIN) sequence across multiple frames - treating
            // one frame as one message is correct for every payload this
            // project has observed (see ADR-0029's Consequences: the
            // reference hub's config/status responses each arrive as a
            // single frame), but would need a FIN check added if a future
            // caller's counterpart ever fragments a message across frames.
            return accumulated;  // last chunk of this frame delivered
        }
        // Large/fragmented frame - loop to collect the rest within the
        // same overall deadline.
    }
}

void HostWebSocketClient::Close() {
    if (curl_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
}

}  // namespace homedeck
