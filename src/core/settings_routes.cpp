#include "core/settings_routes.h"

#include "core/json_request.h"
#include "platform/store_key_validation.h"
#include "third_party/nlohmann/json.hpp"

#include <utility>

namespace homedeck {

namespace {

constexpr const char* kDeviceNameKey = "device_name";

// A defense-in-depth ceiling, not a real settings-size concern - every
// legitimate value this generic API stores today is tiny (a device name,
// a lat/long pair, a module credential), and NVS itself is meant for
// small, frequently-read data (see docs/decisions/ADR-0012-storage-tiers.md),
// not the tier this API is built on. Without this, the only bound on a
// submitted value is the generic kMaxHttpRequestBodyBytes cap every
// endpoint shares, letting an authenticated caller force repeated
// multi-megabyte NVS write attempts against a partition sized for
// kilobytes. Generous enough that no real setting value is ever rejected.
constexpr size_t kMaxSettingValueLength = 4096;

// The same length-plus-path-segment-safety check every SettingsStore
// backend enforces (platform/store_key_validation.h) - checked here too so
// a rejected module/key surfaces as a clean 400 instead of falling through
// to the store's own failure path and a generic 500.
bool InvalidKey(const std::string& module_id, const std::string& key) {
    return !IsValidStoreSegment(module_id) || !IsValidStoreSegment(key);
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
                             DeviceNameValidateFn on_device_name_validate,
                             DeviceNameCommittedFn on_device_name_committed,
                             SettingValidateFn on_setting_validate) {
    server.RegisterHandler(HttpMethod::kGet, "/api/settings", auth.RequireAuth([&storage](const HttpRequest&) {
                                return HttpResponse{200, "application/json", AllSettingsJson(storage).dump(), {}};
                            }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/settings",
        auth.RequireAuth([&storage, on_device_name_validate, on_device_name_committed,
                           on_setting_validate](const HttpRequest& request) {
            auto parsed_opt = TryParseJsonObject(request.body);
            if (!parsed_opt.has_value()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            nlohmann::json& parsed = *parsed_opt;
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
            if (value.size() > kMaxSettingValueLength) {
                return HttpResponse{400, "application/json", R"({"error":"value_too_long"})", {}};
            }
            if (AdminAuthService::IsReservedSettingsKey(module, key)) {
                return HttpResponse{403, "application/json", R"({"error":"reserved_key"})", {}};
            }
            if (on_setting_validate && !on_setting_validate(module, key, value)) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_value"})", {}};
            }
            bool is_device_name = module == AdminAuthService::kModuleId && key == kDeviceNameKey;
            if (is_device_name && on_device_name_validate && !on_device_name_validate(value)) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_value"})", {}};
            }
            if (!storage.SetSetting(module, key, schema_it->get<int>(), value)) {
                return HttpResponse{500, "application/json", R"({"error":"write_failed"})", {}};
            }
            // Only applied once the value above is actually persisted -
            // see DeviceNameCommittedFn's own comment for why the ordering
            // matters.
            if (is_device_name && on_device_name_committed) {
                on_device_name_committed(value);
            }
            return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
        }));

    server.RegisterHandler(
        HttpMethod::kPost, "/api/settings/erase", auth.RequireAuth([&storage](const HttpRequest& request) {
            auto parsed_opt = TryParseJsonObject(request.body);
            if (!parsed_opt.has_value()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            nlohmann::json& parsed = *parsed_opt;
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
            if (AdminAuthService::IsReservedSettingsKey(module, key)) {
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
        HttpMethod::kPost, "/api/backup/restore",
        auth.RequireAuth([&storage, on_setting_validate](const HttpRequest& request) {
            auto parsed_opt = TryParseJsonObject(request.body);
            if (!parsed_opt.has_value()) {
                return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
            }
            nlohmann::json& parsed = *parsed_opt;
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
                if (AdminAuthService::IsReservedSettingsKey(module, key)) {
                    rejected.push_back({{"module", module}, {"key", key}});
                    continue;
                }
                std::string value = value_it->get<std::string>();
                if (InvalidKey(module, key) || value.size() > kMaxSettingValueLength) {
                    failed.push_back({{"module", module}, {"key", key}});
                    continue;
                }
                // Same format check the direct POST /api/settings write
                // path applies (e.g. IsValidHubHost() for Harmony's
                // hub_host) - a restored value that fails it is a policy
                // rejection, not a generic storage fault, the same
                // distinction the reserved-key case above already makes.
                if (on_setting_validate && !on_setting_validate(module, key, value)) {
                    rejected.push_back({{"module", module}, {"key", key}});
                    continue;
                }
                if (storage.SetSetting(module, key, schema_it->get<int>(), value)) {
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
