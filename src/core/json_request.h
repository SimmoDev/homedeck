#pragma once

#include "third_party/nlohmann/json.hpp"

#include <optional>
#include <string>

namespace homedeck {

// The "parse the request body as JSON, reject anything that isn't a
// well-formed JSON object" check every route handler taking a JSON body
// needs before it can look at individual fields - previously duplicated
// verbatim across admin_auth_service.cpp/settings_routes.cpp. Firmware
// builds with exceptions disabled (ESP-IDF's default), so every caller
// already needed nlohmann::json's allow_exceptions=false parse form
// regardless - centralized here so that requirement (and the
// is_discarded()/is_object() check it implies) isn't re-derived at each
// call site.
inline std::optional<nlohmann::json> TryParseJsonObject(const std::string& body) {
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace homedeck
