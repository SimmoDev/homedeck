#pragma once

#include "third_party/nlohmann/json.hpp"

#include <optional>
#include <string>

namespace homedeck {

// Generous multiple of the nesting depth any legitimate body needs across
// this project's JSON APIs - Harmony's own hub-response shapes (data ->
// device[] -> device -> controlGroup[] -> group -> function[] -> function,
// ~7 levels) and the flat settings/backup payloads are both well under it.
constexpr int kMaxJsonNestingDepth = 32;

// nlohmann::json::parse() recurses one C++ stack frame per object/array
// nesting level with no built-in depth bound (this vendored 3.11.3 has
// none). Every caller parsing a JSON body from the network needs this
// checked before parsing: Harmony's hub link has no authentication at all
// (ADR-0029), and the Web UI's own `/api/auth/login`/`/api/auth/setup`
// are themselves the pre-authentication step, reachable by any device on
// the LAN with no credential - either could otherwise be sent a deeply
// nested body to drive a stack overflow, especially against firmware's
// own bounded FreeRTOS task stacks. A plain bracket-counting pre-scan,
// skipping string-literal content (including escaped characters, so a
// brace inside a string value doesn't count) - simpler and more
// self-contained than reimplementing nlohmann's own SAX interface just to
// add a depth cap.
inline bool ExceedsJsonNestingDepth(const std::string& text, int max_depth = kMaxJsonNestingDepth) {
    int depth = 0;
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // skip whatever the escaped character is, including a quote
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) return true;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return false;
}

// The "parse the request body as JSON, reject anything that isn't a
// well-formed JSON object" check every route handler taking a JSON body
// needs before it can look at individual fields - previously duplicated
// verbatim across admin_auth_service.cpp/settings_routes.cpp. Firmware
// builds with exceptions disabled (ESP-IDF's default), so every caller
// already needed nlohmann::json's allow_exceptions=false parse form
// regardless - centralized here so that requirement (and the
// is_discarded()/is_object() check it implies) isn't re-derived at each
// call site. Depth-bounded via ExceedsJsonNestingDepth() for the same
// reason that function documents - every caller here also parses a
// network-supplied request body.
inline std::optional<nlohmann::json> TryParseJsonObject(const std::string& body) {
    if (ExceedsJsonNestingDepth(body)) {
        return std::nullopt;
    }
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace homedeck
