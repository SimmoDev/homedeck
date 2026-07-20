#include "core/ota_routes.h"

#include "core/ota_gate.h"
#include "third_party/nlohmann/json.hpp"

#include <utility>

namespace homedeck {

void RegisterOtaRoutes(HttpServer& server, AdminAuthService& auth, BatteryReader& battery_reader, OtaWriter writer,
                        OtaRebootFn reboot) {
    server.RegisterHandler(
        HttpMethod::kGet, "/api/ota/status",
        auth.RequireAuth([&battery_reader, writer](const HttpRequest&) {
            OtaGateStatus gate = EvaluateOtaGate(battery_reader);
            nlohmann::json body = {
                {"batteryPercent", battery_reader.ReadPercent()},
                {"externalPowerConnected", battery_reader.IsExternalPowerConnected()},
                {"batteryPresent", battery_reader.IsBatteryPresent()},
                {"gateOpen", gate.open},
                {"gateReason", gate.reason},
                {"runningVersion", writer.running_version()},
            };
            return HttpResponse{200, "application/json", body.dump(), {}};
        }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/ota/upload",
        auth.RequireAuth([&battery_reader, writer](const HttpRequest& request) {
            OtaGateStatus gate = EvaluateOtaGate(battery_reader);
            if (!gate.open) {
                nlohmann::json body = {{"error", "gate_closed"}, {"reason", gate.reason}};
                return HttpResponse{403, "application/json", body.dump(), {}};
            }
            if (request.body.size() > writer.max_image_size()) {
                nlohmann::json body = {{"error", "image_too_large"}};
                return HttpResponse{400, "application/json", body.dump(), {}};
            }
            if (!writer.write_image(request.body)) {
                nlohmann::json body = {{"error", "write_failed"}};
                return HttpResponse{500, "application/json", body.dump(), {}};
            }
            return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
        }));

    server.RegisterHandler(HttpMethod::kPost, "/api/ota/reboot",
                            auth.RequireAuth([reboot](const HttpRequest&) {
                                reboot();
                                return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
                            }));
}

}  // namespace homedeck
