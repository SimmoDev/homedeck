#pragma once

#include <optional>
#include <string>

namespace homedeck {

// Decodes application/x-www-form-urlencoded text: "%XX" -> the raw byte,
// '+' -> space. Shared by weather_routes.cpp's GET query-string parsing
// and wifi_setup.cpp's POST form-body parsing - both are this same
// encoding, and ESP-IDF's httpd_query_key_value() does not decode it
// (confirmed against esp_http_server's own source), so a caller reading
// either has to do this itself.
std::string UrlDecode(const std::string& text);

// Extracts and decodes one "key=value" field out of an
// application/x-www-form-urlencoded string ("key1=val1&key2=val2..."),
// whether it's a POST body or a GET query string. Returns std::nullopt
// if the key isn't present at all.
std::optional<std::string> ParseFormField(const std::string& form_data, const std::string& key);

}  // namespace homedeck
