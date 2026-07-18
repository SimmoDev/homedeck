#include "platform/firmware/http_server.h"

#include <algorithm>

namespace homedeck {

namespace {

std::string StatusLine(int status_code) {
    switch (status_code) {
        case 200:
            return "200 OK";
        case 404:
            return "404 Not Found";
        case 405:
            return "405 Method Not Allowed";
        case 500:
            return "500 Internal Server Error";
        default:
            return std::to_string(status_code);
    }
}

}  // namespace

FirmwareHttpServer::~FirmwareHttpServer() { Stop(); }

void FirmwareHttpServer::RegisterHandler(HttpMethod method, const std::string& path, Handler handler) {
    handlers_[{method, path}] = std::move(handler);
}

bool FirmwareHttpServer::Start(uint16_t port) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = std::max<size_t>(handlers_.size(), 1);

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
        httpd_register_uri_handler(server_, &uri);
    }
    return true;
}

void FirmwareHttpServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
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
        request.body.resize(req->content_len);
        int received = httpd_req_recv(req, request.body.data(), request.body.size());
        if (received <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    HttpResponse response = (*handler)(request);

    httpd_resp_set_status(req, StatusLine(response.status_code).c_str());
    httpd_resp_set_type(req, response.content_type.c_str());
    httpd_resp_send(req, response.body.data(), response.body.size());
    return ESP_OK;
}

}  // namespace homedeck
