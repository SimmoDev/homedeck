#include "core/url_codec.h"

#include <cctype>

namespace homedeck {

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

std::optional<std::string> ParseFormField(const std::string& form_data, const std::string& key) {
    std::string prefix = key + "=";
    // Anchored to a field boundary (start of string, or right after '&') -
    // a plain form_data.find(prefix) would also match "key=" appearing
    // inside a *value* (e.g. password=ssid=x&ssid=RealNetwork would wrongly
    // extract "x&ssid" for "ssid" instead of "RealNetwork").
    size_t pos = 0;
    while ((pos = form_data.find(prefix, pos)) != std::string::npos) {
        if (pos == 0 || form_data[pos - 1] == '&') {
            size_t start = pos + prefix.size();
            size_t end = form_data.find('&', start);
            return UrlDecode(form_data.substr(start, end == std::string::npos ? std::string::npos : end - start));
        }
        pos += 1;
    }
    return std::nullopt;
}

}  // namespace homedeck
