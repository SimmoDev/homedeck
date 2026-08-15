#pragma once

#include "platform/http_client.h"

namespace homedeck {

// Backs HttpClient with libcurl - a system dependency (find_package(CURL)
// in src/CMakeLists.txt), not FetchContent-vendored like civetweb/mbedtls,
// since this is host-only dev tooling never shipped to firmware, the
// same precedent DEVELOPMENT.md already sets for SDL2.
class HostHttpClient : public HttpClient {
public:
    HttpClientResponse Get(const std::string& url) override;
    HttpClientResponse Post(const std::string& url, const std::string& json_body,
                             const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) override;
};

}  // namespace homedeck
