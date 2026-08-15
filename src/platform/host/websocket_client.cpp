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
    size_t sent = 0;
    CURLcode result = curl_ws_send(static_cast<CURL*>(curl_), text.data(), text.size(), &sent, 0, CURLWS_TEXT);
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
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return std::nullopt;
        }

        struct pollfd pfd = {};
        pfd.fd = sockfd;
        pfd.events = POLLIN;
        int poll_result = poll(&pfd, 1, static_cast<int>(remaining.count()));
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
        accumulated.append(buffer, received);
        if (meta->bytesleft == 0) {
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
