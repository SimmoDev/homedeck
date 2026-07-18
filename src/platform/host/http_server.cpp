#include "platform/host/http_server.h"

#include <civetweb.h>

#include <set>

namespace homedeck {

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
        mg_read(conn, request.body.data(), request.body.size());
    }

    HttpResponse response = it->second(request);

    mg_send_http_ok(conn, response.content_type.c_str(), static_cast<long long>(response.body.size()));
    mg_write(conn, response.body.data(), response.body.size());
    return 1;
}

}  // namespace homedeck
