#include "core/diagnostics_routes.h"

#include "third_party/nlohmann/json.hpp"

#include <utility>

namespace homedeck {

namespace {
constexpr const char* kModuleId = "core";
}  // namespace

void RegisterDiagnosticsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                                BatteryReader& battery_reader, CoreDumpReader read_core_dump) {
    server.RegisterHandler(
        HttpMethod::kGet, "/api/diagnostics",
        auth.RequireAuth([&storage, &battery_reader](const HttpRequest&) {
            auto reset_reason = storage.GetSetting(kModuleId, "reset_reason");
            auto has_core_dump = storage.GetSetting(kModuleId, "has_core_dump");
            nlohmann::json body = {
                {"resetReason", reset_reason.has_value() ? reset_reason->value : "unknown"},
                {"hasCoreDump", has_core_dump.has_value() && has_core_dump->value == "true"},
                {"batteryPercent", battery_reader.ReadPercent()},
                {"externalPowerConnected", battery_reader.IsExternalPowerConnected()},
                {"batteryPresent", battery_reader.IsBatteryPresent()},
            };
            return HttpResponse{200, "application/json", body.dump(), {}};
        }));

    server.RegisterHandler(
        HttpMethod::kGet, "/api/diagnostics/coredump",
        auth.RequireAuth([read_core_dump](const HttpRequest&) {
            auto core_dump = read_core_dump();
            if (!core_dump.has_value()) {
                return HttpResponse{404, "application/json", R"({"error":"not_found"})", {}};
            }
            HttpResponse response{200, "application/octet-stream", std::move(*core_dump), {}};
            response.extra_headers.push_back({"Content-Disposition", "attachment; filename=\"coredump.bin\""});
            return response;
        }));
}

}  // namespace homedeck
