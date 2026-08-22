#include "platform/firmware/http_server.h"

#include "esp_log.h"

#include <algorithm>
#include <vector>

namespace homedeck {

namespace {

constexpr char kTag[] = "http_server";

// httpd_resp_set_status() sends this string as-is after "HTTP/1.1 " - it
// must never be a bare number (e.g. "409"), since RFC 7230's status-line
// grammar requires a reason-phrase after the status code and some HTTP
// clients handle a missing one poorly (see DispatchTrampoline's own
// comment on this same status line for a different malformed-status-line
// failure this project already hit on real hardware). The code->phrase
// table itself is shared with the host backend - see HttpReasonPhrase()
// (platform/http_server.h).
std::string StatusLine(int status_code) {
    return std::to_string(status_code) + " " + HttpReasonPhrase(status_code);
}

// httpd_req_recv()'s HTTPD_SOCK_ERR_TIMEOUT is retryable, but retrying it
// forever isn't: a client that opens a connection, sends Content-Length,
// then goes silent would otherwise stall this loop indefinitely, blocking
// the one httpd worker thread from serving any other request (auth, OTA,
// diagnostics - everything shares this single server instance). At the
// default 5s recv_wait_timeout (HTTPD_DEFAULT_CONFIG()), this allows 15s
// of total silence - generous for a slow but alive multi-MB OTA upload,
// since the counter resets on every call that actually receives data.
constexpr int kMaxConsecutiveRecvTimeouts = 3;
// kMaxConsecutiveRecvTimeouts alone bounds silence, not total transfer
// time - a client trickling in a single byte just under every timeout
// would reset that counter forever and hold the worker thread for as
// long as it likes, up to kMaxHttpRequestBodyBytes. This is a separate,
// never-reset budget across the whole request: 120 timeouts (~10 minutes
// at the default 5s wait) comfortably covers even a genuinely slow
// multi-MB OTA upload over a weak link, while still bounding the
// otherwise-unbounded worst case above.
constexpr int kMaxTotalRecvTimeouts = 120;

}  // namespace

FirmwareHttpServer::~FirmwareHttpServer() { Stop(); }

void FirmwareHttpServer::RegisterHandler(HttpMethod method, const std::string& path, Handler handler) {
    handlers_[{method, path}] = std::move(handler);
}

bool FirmwareHttpServer::Start(uint16_t port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = std::max<size_t>(handlers_.size(), 1);
    // HTTPD_DEFAULT_CONFIG()'s 4096-byte default overflows once a
    // handler nests JSON parsing plus AdminAuthService's PBKDF2/mbedtls
    // call stack inside it - see docs/architecture/web-ui.md#status.
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        return false;
    }

    for (auto& [key, handler] : handlers_) {
        httpd_uri_t uri = {
            .uri = key.second.c_str(),
            .method = key.first == HttpMethod::kGet ? HTTP_GET : HTTP_POST,
            .handler = &FirmwareHttpServer::DispatchTrampoline,
            .user_ctx = &handler,
        };
        if (httpd_register_uri_handler(server_, &uri) != ESP_OK) {
            ESP_LOGW(kTag, "Failed to register route: %s", key.second.c_str());
        }
    }
    return true;
}

void FirmwareHttpServer::Stop() {
    if (server_ != nullptr) {
        esp_err_t err = httpd_stop(server_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "httpd_stop failed: %s", esp_err_to_name(err));
        }
        server_ = nullptr;
    }
}

esp_err_t FirmwareHttpServer::DispatchTrampoline(httpd_req_t* req) {
    auto* handler = static_cast<Handler*>(req->user_ctx);

    HttpRequest request;
    std::string full_uri = req->uri;
    size_t query_start = full_uri.find('?');
    if (query_start != std::string::npos) {
        request.path = full_uri.substr(0, query_start);
        request.query = full_uri.substr(query_start + 1);
    } else {
        request.path = full_uri;
    }

    if (req->content_len > 0) {
        if (static_cast<size_t>(req->content_len) > kMaxHttpRequestBodyBytes) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            httpd_resp_send(req, nullptr, 0);
            return ESP_FAIL;
        }
        request.body.resize(req->content_len);
        // A single httpd_req_recv() call is not guaranteed to return the
        // full body - fine for the small JSON bodies used so far, a real
        // bug for anything larger (e.g. an OTA image upload), which
        // arrives across multiple underlying reads. Loop until the whole
        // body is read; HTTPD_SOCK_ERR_TIMEOUT is retryable per
        // esp_http_server's own documented recv() contract, any other
        // negative result is a real failure.
        size_t total_received = 0;
        int consecutive_timeouts = 0;
        int total_timeouts = 0;
        while (total_received < request.body.size()) {
            int received = httpd_req_recv(req, request.body.data() + total_received,
                                           request.body.size() - total_received);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++consecutive_timeouts > kMaxConsecutiveRecvTimeouts ||
                    ++total_timeouts > kMaxTotalRecvTimeouts) {
                    httpd_resp_send_500(req);
                    return ESP_FAIL;
                }
                continue;
            }
            if (received <= 0) {
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            consecutive_timeouts = 0;
            total_received += static_cast<size_t>(received);
        }
    }

    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len > 0) {
        // httpd_req_get_hdr_value_str wants the buffer sized for the
        // value plus its null terminator - a std::vector<char> here
        // rather than std::string::data(), since writing a trailing
        // null one past a string's declared size is undefined behavior.
        std::vector<char> cookie_buffer(cookie_len + 1);
        if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buffer.data(), cookie_buffer.size()) == ESP_OK) {
            request.cookie_header = std::string(cookie_buffer.data());
        }
    }

    HttpResponse response = (*handler)(request);

    // httpd_resp_set_status() only stores the pointer it's given
    // (ra->status = (char*)status in ESP-IDF's own httpd_txrx.c) - it
    // doesn't send anything until httpd_resp_send() below, so the
    // string this points to must outlive that call. A temporary
    // (StatusLine(...).c_str() inline) would be destroyed at the end of
    // this statement, leaving a dangling pointer read moments later -
    // intermittent HTTP response corruption (curl: "Unsupported HTTP/1
    // subversion in response").
    std::string status_line = StatusLine(response.status_code);
    httpd_resp_set_status(req, status_line.c_str());
    httpd_resp_set_type(req, response.content_type.c_str());
    for (const auto& [name, value] : response.extra_headers) {
        httpd_resp_set_hdr(req, name.c_str(), value.c_str());
    }
    httpd_resp_send(req, response.body.data(), response.body.size());
    return ESP_OK;
}

}  // namespace homedeck
