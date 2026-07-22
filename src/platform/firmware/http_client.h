#pragma once

#include "platform/http_client.h"

namespace homedeck {

// Backs HttpClient with esp_http_client, TLS verified via ESP-IDF's
// built-in certificate bundle (esp_crt_bundle_attach) rather than a
// pinned cert that would need manual rotation - see
// docs/architecture/networking.md for the RPC-timeout risk this is the
// first thing to exercise on the ESP32-C6/esp_wifi_remote link.
class FirmwareHttpClient : public HttpClient {
public:
    HttpClientResponse Get(const std::string& url) override;
};

}  // namespace homedeck
