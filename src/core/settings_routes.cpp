#include "core/settings_routes.h"

#include "platform/store_key_validation.h"
#include "third_party/nlohmann/json.hpp"

#include <utility>

namespace homedeck {

namespace {

constexpr const char* kDeviceNameKey = "device_name";

// The same length-plus-path-segment-safety check every SettingsStore
// backend enforces (platform/store_key_validation.h) - checked here too so
// a rejected module/key surfaces as a clean 400 instead of falling through
// to the store's own failure path and a generic 500.
bool InvalidKey(const std::string& module_id, const std::string& key) {
    return !IsValidStoreSegment(module_id) || !IsValidStoreSegment(key);
}

// Storage::SetSetting already rejects this exact pair (see storage.cpp's
// IsReservedForSecrets, ADR-0023/ADR-0027) - checked here too so a
// deliberate security rejection surfaces as its own distinguishable
// response rather than the generic 500 "write_failed" a real storage
// fault also produces, which would otherwise make the two
// indistinguishable in logs/monitoring.
bool IsReservedKey(const std::string& module_id, const std::string& key) {
    return module_id == AdminAuthService::kModuleId && key == AdminAuthService::kPasswordKey;
}

nlohmann::json EntryToJson(const SettingEntry& entry) {
    return {
        {"module", entry.module_id},
        {"key", entry.key},
        {"value", entry.value},
        {"schemaVersion", entry.schema_version},
    };
}

nlohmann::json AllSettingsJson(Storage& storage) {
    nlohmann::json entries = nlohmann::json::array();
    for (const SettingEntry& entry : storage.ListAllSettings()) {
        entries.push_back(EntryToJson(entry));
    }
    return entries;
}

}  // namespace

void RegisterSettingsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                             DeviceNameChangedFn on_device_name_changed) {
    server.RegisterHandler(HttpMethod::kGet, "/api/settings", auth.RequireAuth([&storage](const HttpRequest&) {
                                return HttpResponse{200, "application/json", AllSettingsJson(storage).dump(), {}};
                            }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/settings",
        auth.RequireAuth([&storage, on_device_name_changed](const HttpRequest& request) {
            nlohmann::json parsed = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            auto module_it = parsed.find("module");
            auto key_it = parsed.find("key");
            auto value_it = parsed.find("value");
            auto schema_it = parsed.find("schemaVersion");
            if (module_it == parsed.end() || !module_it->is_string()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"module"})", {}};
            }
            if (key_it == parsed.end() || !key_it->is_string()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"key"})", {}};
            }
            if (value_it == parsed.end() || !value_it->is_string()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"value"})", {}};
            }
            if (schema_it == parsed.end() || !schema_it->is_number_integer()) {
                return HttpResponse{
                    400, "application/json", R"({"error":"missing_field","field":"schemaVersion"})", {}};
            }
            std::string module = module_it->get<std::string>();
            std::string key = key_it->get<std::string>();
            std::string value = value_it->get<std::string>();
            if (InvalidKey(module, key)) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_key"})", {}};
            }
            if (IsReservedKey(module, key)) {
                return HttpResponse{403, "application/json", R"({"error":"reserved_key"})", {}};
            }
            if (module == AdminAuthService::kModuleId && key == kDeviceNameKey && on_device_name_changed) {
                if (!on_device_name_changed(value)) {
                    return HttpResponse{400, "application/json", R"({"error":"invalid_value"})", {}};
                }
            }
            if (!storage.SetSetting(module, key, schema_it->get<int>(), value)) {
                return HttpResponse{500, "application/json", R"({"error":"write_failed"})", {}};
            }
            return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
        }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/settings/erase", auth.RequireAuth([&storage](const HttpRequest& request) {
            nlohmann::json parsed = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            auto module_it = parsed.find("module");
            auto key_it = parsed.find("key");
            if (module_it == parsed.end() || !module_it->is_string() || key_it == parsed.end() ||
                !key_it->is_string()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field"})", {}};
            }
            std::string module = module_it->get<std::string>();
            std::string key = key_it->get<std::string>();
            if (InvalidKey(module, key)) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_key"})", {}};
            }
            if (IsReservedKey(module, key)) {
                return HttpResponse{403, "application/json", R"({"error":"reserved_key"})", {}};
            }
            if (!storage.EraseSetting(module, key)) {
                return HttpResponse{500, "application/json", R"({"error":"erase_failed"})", {}};
            }
            return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
        }));

    server.RegisterHandler(HttpMethod::kGet, "/api/backup", auth.RequireAuth([&storage](const HttpRequest&) {
                                nlohmann::json body = {{"settings", AllSettingsJson(storage)}};
                                HttpResponse response{200, "application/json", body.dump(), {}};
                                response.extra_headers.push_back(
                                    {"Content-Disposition", "attachment; filename=\"homedeck-backup.json\""});
                                return response;
                            }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/backup/restore", auth.RequireAuth([&storage](const HttpRequest& request) {
            nlohmann::json parsed = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            auto settings_it = parsed.find("settings");
            if (settings_it == parsed.end() || !settings_it->is_array()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"settings"})", {}};
            }
            // Not atomic - no transaction concept exists anywhere else in
            // this codebase either. Each entry either applies or is
            // reported back, so a partial restore is visible rather than
            // silently incomplete.
            int applied = 0;
            nlohmann::json failed = nlohmann::json::array();
            // Kept separate from failed - a reserved-key entry (e.g. a
            // backup containing admin_pw_hash) is a deliberate security
            // rejection, the same distinguishable case the direct
            // POST /api/settings endpoint gives its own 403 "reserved_key"
            // for. Without this, Storage::SetSetting's own internal guard
            // would still block it, but it would land in failed
            // indistinguishably from a genuine storage fault.
            nlohmann::json rejected = nlohmann::json::array();
            for (const auto& entry : *settings_it) {
                if (!entry.is_object()) continue;
                auto module_it = entry.find("module");
                auto key_it = entry.find("key");
                auto value_it = entry.find("value");
                auto schema_it = entry.find("schemaVersion");
                if (module_it == entry.end() || !module_it->is_string() || key_it == entry.end() ||
                    !key_it->is_string() || value_it == entry.end() || !value_it->is_string() ||
                    schema_it == entry.end() || !schema_it->is_number_integer()) {
                    continue;
                }
                std::string module = module_it->get<std::string>();
                std::string key = key_it->get<std::string>();
                if (IsReservedKey(module, key)) {
                    rejected.push_back({{"module", module}, {"key", key}});
                    continue;
                }
                bool ok = !InvalidKey(module, key) &&
                          storage.SetSetting(module, key, schema_it->get<int>(), value_it->get<std::string>());
                if (ok) {
                    ++applied;
                } else {
                    failed.push_back({{"module", module}, {"key", key}});
                }
            }
            nlohmann::json body = {
                {"applied", applied}, {"failed", std::move(failed)}, {"rejected", std::move(rejected)}};
            return HttpResponse{200, "application/json", body.dump(), {}};
        }));
}

}  // namespace homedeck
