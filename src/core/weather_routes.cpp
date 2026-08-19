#include "core/weather_routes.h"

#include "core/json_request.h"
#include "core/url_codec.h"
#include "third_party/nlohmann/json.hpp"

#include <cctype>
#include <optional>

namespace homedeck {

namespace {

// A defense-in-depth ceiling, not a real search-usability concern - every
// other user-supplied field this milestone added gets an explicit length
// check; this is the one that didn't, bounded only by the HTTP server's
// own URI-length limit. Generous enough that no real place name is ever
// rejected.
constexpr size_t kMaxGeocodeQueryLength = 100;

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

}  // namespace

void RegisterWeatherRoutes(HttpServer& server, HttpClient& http_client, OpenMeteoWeatherProvider& weather_provider,
                            AdminAuthService& auth) {
    server.RegisterHandler(
        HttpMethod::kGet, "/api/weather/geocode",
        auth.RequireAuth([&http_client](const HttpRequest& request) {
            auto query = ParseFormField(request.query, "query");
            if (!query.has_value() || query->empty()) {
                return HttpResponse{400, "application/json", R"({"error":"missing_field","field":"query"})", {}};
            }
            if (query->size() > kMaxGeocodeQueryLength) {
                return HttpResponse{400, "application/json", R"({"error":"query_too_long"})", {}};
            }

            std::string url =
                "https://geocoding-api.open-meteo.com/v1/search?name=" + UrlEncode(*query) + "&count=10";
            HttpClientResponse response = http_client.Get(url);
            if (!response.success || response.status_code != 200) {
                return HttpResponse{502, "application/json", R"({"error":"upstream_failed"})", {}};
            }

            std::optional<nlohmann::json> parsed = TryParseJsonObject(response.body);
            if (!parsed.has_value()) {
                return HttpResponse{502, "application/json", R"({"error":"upstream_invalid_response"})", {}};
            }

            nlohmann::json results = nlohmann::json::array();
            auto upstream_results = parsed->find("results");
            if (upstream_results != parsed->end() && upstream_results->is_array()) {
                for (const auto& entry : *upstream_results) {
                    // "name" must both be present and string-typed, same
                    // as latitude/longitude below must be present and
                    // number-typed - entry.value()/entry.get<T>() throw
                    // json::type_error on a type mismatch, which is
                    // std::abort() on firmware (exceptions are disabled
                    // there), not a catchable error.
                    if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string() ||
                        !entry.contains("latitude") || !entry.contains("longitude") ||
                        !entry.at("latitude").is_number() || !entry.at("longitude").is_number()) {
                        continue;
                    }
                    // admin1/country are optional, unlike name/latitude/
                    // longitude above - a type mismatch on one of these
                    // shouldn't drop an otherwise-valid result, so this
                    // defaults to empty instead of skipping the entry.
                    auto optional_string = [&entry](const char* key) -> std::string {
                        auto it = entry.find(key);
                        return (it != entry.end() && it->is_string()) ? it->get<std::string>() : "";
                    };
                    results.push_back({
                        {"name", entry.at("name").get<std::string>()},
                        {"admin1", optional_string("admin1")},
                        {"country", optional_string("country")},
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
