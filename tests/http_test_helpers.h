#pragma once

// Shared raw-socket HTTP test client for tests/*_routes_test.cpp - proves
// each HttpServer-backed route actually round-trips over a real TCP
// connection, not just that handler logic runs in isolation. Header-only
// (inline) since it's included directly by several test translation units
// that all link into the same tests/ binary.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace homedeck::testing {

struct HttpResult {
    int status_code = 0;
    std::string set_cookie;
    std::string body;
};

inline HttpResult HttpRequestRaw(uint16_t port, const std::string& method, const std::string& path,
                                  const std::string& body, const std::string& cookie_header = "") {
    HttpResult result;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return result;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return result;
    }

    std::string request = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!cookie_header.empty()) {
        request += "Cookie: " + cookie_header + "\r\n";
    }
    if (!body.empty()) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n" + body;

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

    size_t header_end = response.find("\r\n\r\n");
    std::string headers = header_end == std::string::npos ? response : response.substr(0, header_end);
    result.body = header_end == std::string::npos ? "" : response.substr(header_end + 4);

    size_t status_start = headers.find(' ');
    if (status_start != std::string::npos) {
        result.status_code = std::atoi(headers.c_str() + status_start + 1);
    }
    size_t cookie_pos = headers.find("Set-Cookie: ");
    if (cookie_pos != std::string::npos) {
        size_t value_start = cookie_pos + std::strlen("Set-Cookie: ");
        size_t line_end = headers.find("\r\n", value_start);
        result.set_cookie = headers.substr(value_start, line_end - value_start);
    }
    return result;
}

// Extracts just the "session=<token>" part of a Set-Cookie value (which
// also carries HttpOnly/SameSite/Path/Max-Age) for use as a request's
// Cookie header.
inline std::string SessionCookieOnly(const std::string& set_cookie) {
    size_t semicolon = set_cookie.find(';');
    return semicolon == std::string::npos ? set_cookie : set_cookie.substr(0, semicolon);
}

}  // namespace homedeck::testing
