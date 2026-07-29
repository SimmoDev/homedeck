#include "core/weather_routes.h"

#include "third_party/nlohmann/json.hpp"

#include <cctype>
#include <optional>

namespace homedeck {

namespace {

std::string UrlDecode(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        return std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            result += static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2]));
            i += 2;
        } else if (text[i] == '+') {
            result += ' ';
        } else {
            result += text[i];
        }
    }
    return result;
}

// RFC 3986 percent-encoding for a query component - needed because
// *query (below) is the already-decoded incoming value, and it gets
// forwarded as a query parameter on a *new* outbound URL to Open-Meteo,
// not echoed back verbatim. A raw space/comma left unescaped there
// isn't just cosmetically wrong: a literal unescaped space splits what
// libcurl parses as the URL, breaking the request before it's even
// sent, and Open-Meteo's own server rejects one as malformed either way.
std::string UrlEncode(const std::string& text) {
    static const char kHexDigits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += kHexDigits[c >> 4];
            result += kHexDigits[c & 0x0F];
        }
    }
    return result;
}

// No shared query-string parser exists yet (HttpRequest::query is the
// raw string - every other endpoint so far only reads POST bodies, not
// GET query params) - a small local helper rather than a new Core
// utility with only this one caller.
std::optional<std::string> GetQueryParam(const std::string& query_string, const std::string& key) {
    std::string prefix = key + "=";
    size_t pos = query_string.find(prefix);
    if (pos == std::string::npos) return std::nullopt;
    size_t start = pos + prefix.size();
    size_t end = query_string.find('&', start);
    return UrlDecode(query_string.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

}  // namespace

void RegisterWeatherRoutes(HttpServer& server, HttpClient& http_client, OpenMeteoWeatherProvider& weather_provider,
                            AdminAuthService& auth) {
    server.RegisterHandler(
        HttpMethod::kGet, "/api/weather/geocode",
        auth.RequireAuth([&http_client](const HttpRequest& request) {
            auto query = GetQueryParam(request.query, "query");
            if (!query.has_value() || query->empty()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"query"})", {}};
            }

            std::string url =
                "https://geocoding-api.open-meteo.com/v1/search?name=" + UrlEncode(*query) + "&count=10";
            HttpClientResponse response = http_client.Get(url);
            if (!response.success || response.status_code != 200) {
                return HttpResponse{502, "application/json", R"({"error":"upstream_failed"})", {}};
            }

            nlohmann::json parsed = nlohmann::json::parse(response.body, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                return HttpResponse{502, "application/json", R"({"error":"upstream_invalid_response"})", {}};
            }

            nlohmann::json results = nlohmann::json::array();
            auto upstream_results = parsed.find("results");
            if (upstream_results != parsed.end() && upstream_results->is_array()) {
                for (const auto& entry : *upstream_results) {
                    if (!entry.is_object() || !entry.contains("name") || !entry.contains("latitude") ||
                        !entry.contains("longitude") || !entry.at("latitude").is_number() ||
                        !entry.at("longitude").is_number()) {
                        continue;
                    }
                    results.push_back({
                        {"name", entry.value("name", "")},
                        {"admin1", entry.value("admin1", "")},
                        {"country", entry.value("country", "")},
                        {"latitude", entry.at("latitude").get<double>()},
                        {"longitude", entry.at("longitude").get<double>()},
                    });
                }
            }

            nlohmann::json body = {{"results", results}};
            return HttpResponse{200, "application/json", body.dump(), {}};
        }));

    server.RegisterHandler(HttpMethod::kPost, "/api/weather/refresh",
                            auth.RequireAuth([&weather_provider](const HttpRequest&) {
                                weather_provider.TriggerPoll();
                                return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
                            }));
}

}  // namespace homedeck
