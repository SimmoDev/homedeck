#include "core/harmony_routes.h"

#include "third_party/nlohmann/json.hpp"

namespace homedeck {

namespace {

const char* StateToString(HarmonyConnectionState state) {
    switch (state) {
        case HarmonyConnectionState::kDisconnected:
            return "disconnected";
        case HarmonyConnectionState::kConnecting:
            return "connecting";
        case HarmonyConnectionState::kConnected:
            return "connected";
        case HarmonyConnectionState::kError:
            return "error";
    }
    return "disconnected";
}

nlohmann::json SnapshotToJson(const HarmonyConnectionSnapshot& snapshot) {
    nlohmann::json devices = nlohmann::json::array();
    for (const HarmonyDevice& device : snapshot.devices) {
        devices.push_back({{"id", device.id}, {"label", device.label}});
    }
    nlohmann::json activities = nlohmann::json::array();
    for (const HarmonyActivity& activity : snapshot.activities) {
        activities.push_back({{"id", activity.id}, {"label", activity.label}});
    }
    return {
        {"state", StateToString(snapshot.state)},
        {"hasConfig", snapshot.has_config},
        {"devices", std::move(devices)},
        {"activities", std::move(activities)},
    };
}

}  // namespace

void RegisterHarmonyRoutes(HttpServer& server, HarmonyConnection& harmony_connection, AdminAuthService& auth) {
    server.RegisterHandler(HttpMethod::kGet, "/api/harmony/status",
                            auth.RequireAuth([&harmony_connection](const HttpRequest&) {
                                return HttpResponse{200, "application/json",
                                                     SnapshotToJson(harmony_connection.Snapshot()).dump(), {}};
                            }));

    server.RegisterHandler(HttpMethod::kPost, "/api/harmony/reconnect",
                            auth.RequireAuth([&harmony_connection](const HttpRequest&) {
                                harmony_connection.TriggerReconnect();
                                return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
                            }));
}

}  // namespace homedeck
