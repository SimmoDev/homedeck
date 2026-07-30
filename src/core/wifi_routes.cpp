#include "core/wifi_routes.h"

#include "third_party/nlohmann/json.hpp"

namespace homedeck {

void RegisterWifiRoutes(HttpServer& server, AdminAuthService& auth, WifiResetFn reset) {
    server.RegisterHandler(HttpMethod::kPost, "/api/wifi/reset", auth.RequireAuth([reset](const HttpRequest&) {
                                std::optional<std::string> ap_ssid = reset();
                                if (!ap_ssid.has_value()) {
                                    return HttpResponse{500, "application/json", R"({"error":"reset_failed"})", {}};
                                }
                                nlohmann::json body = {{"status", "ok"}, {"apSsid", *ap_ssid}};
                                return HttpResponse{200, "application/json", body.dump(), {}};
                            }));
}

}  // namespace homedeck
