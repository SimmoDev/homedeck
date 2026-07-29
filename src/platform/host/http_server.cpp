#include "platform/host/http_server.h"

#include <civetweb.h>

#include <set>

namespace homedeck {

namespace {

// mg_send_http_ok() can't carry extra headers, so responses are written
// by hand here instead - status line, Content-Type/Content-Length (which
// mg_send_http_ok would otherwise generate), any extra headers, then the
// body. Only the status codes this project's handlers actually return
// need a reason phrase; anything else falls back to a generic one rather
// than growing this list speculatively.
const char* ReasonPhrase(int status_code) {
    switch (status_code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 409:
            return "Conflict";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 502:
            return "Bad Gateway";
        default:
            return "Unknown";
    }
}

}  // namespace

HostHttpServer::HostHttpServer() = default;

HostHttpServer::~HostHttpServer() { Stop(); }

void HostHttpServer::RegisterHandler(HttpMethod method, const std::string& path, Handler handler) {
    handlers_[{method, path}] = std::move(handler);
}

bool HostHttpServer::Start(uint16_t port) {
    std::string port_str = std::to_string(port);
    const char* options[] = {"listening_ports", port_str.c_str(), "num_threads", "2", nullptr};
    ctx_ = mg_start(nullptr, this, options);
    if (ctx_ == nullptr) {
        return false;
    }

    std::set<std::string> paths;
    for (const auto& [key, handler] : handlers_) {
        paths.insert(key.second);
    }
    for (const std::string& path : paths) {
        mg_set_request_handler(ctx_, path.c_str(), &HostHttpServer::DispatchTrampoline, this);
    }
    return true;
}

void HostHttpServer::Stop() {
    if (ctx_ != nullptr) {
        mg_stop(ctx_);
        ctx_ = nullptr;
    }
}

int HostHttpServer::DispatchTrampoline(mg_connection* conn, void* cbdata) {
    return static_cast<HostHttpServer*>(cbdata)->Dispatch(conn);
}

int HostHttpServer::Dispatch(mg_connection* conn) {
    const mg_request_info* info = mg_get_request_info(conn);

    HttpMethod method;
    std::string method_str = info->request_method != nullptr ? info->request_method : "";
    if (method_str == "GET") {
        method = HttpMethod::kGet;
    } else if (method_str == "POST") {
        method = HttpMethod::kPost;
    } else {
        mg_send_http_error(conn, 405, "Method Not Allowed");
        return 1;
    }

    auto it = handlers_.find({method, info->request_uri});
    if (it == handlers_.end()) {
        mg_send_http_error(conn, 404, "Not Found");
        return 1;
    }

    HttpRequest request;
    request.path = info->request_uri;
    request.query = info->query_string != nullptr ? info->query_string : "";
    if (info->content_length > 0) {
        request.body.resize(static_cast<size_t>(info->content_length));
        // A single mg_read() call is not guaranteed to return the full
        // body - fine for the small JSON bodies used so far, a real bug
        // for anything larger (e.g. an OTA image upload), which arrives
        // across multiple underlying reads. Loop until the whole body is
        // read; per mg_read()'s documented contract, 0 means the peer
        // closed the connection and negative means a read error - both
        // are unrecoverable here.
        size_t total_received = 0;
        while (total_received < request.body.size()) {
            int received = mg_read(conn, request.body.data() + total_received,
                                    request.body.size() - total_received);
            if (received <= 0) {
                mg_send_http_error(conn, 500, "Internal Server Error");
                return 1;
            }
            total_received += static_cast<size_t>(received);
        }
    }
    const char* cookie_header = mg_get_header(conn, "Cookie");
    if (cookie_header != nullptr) {
        request.cookie_header = cookie_header;
    }

    HttpResponse response = it->second(request);

    mg_printf(conn, "HTTP/1.1 %d %s\r\n", response.status_code, ReasonPhrase(response.status_code));
    mg_printf(conn, "Content-Type: %s\r\n", response.content_type.c_str());
    mg_printf(conn, "Content-Length: %zu\r\n", response.body.size());
    for (const auto& [name, value] : response.extra_headers) {
        mg_printf(conn, "%s: %s\r\n", name.c_str(), value.c_str());
    }
    mg_printf(conn, "\r\n");
    mg_write(conn, response.body.data(), response.body.size());
    return 1;
}

}  // namespace homedeck
