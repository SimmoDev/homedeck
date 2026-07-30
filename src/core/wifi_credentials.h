#pragma once

#include <string>

namespace homedeck {

// esp_wifi's own fixed-size wifi_sta_config_t.ssid/.password buffers are
// 32/64 bytes (esp_wifi_types_generic.h), but wifi_setup.cpp fills them
// via snprintf(..., "%s", ...) rather than a length-prefixed copy - there
// is no separate ssid_len field for STA config the way there is for AP -
// so one byte of each buffer is reserved for the null terminator.
// 63 also happens to be WPA2-PSK's own real passphrase length limit, not
// just an snprintf accommodation. Duplicated here as plain constants
// rather than pulled in via an esp_wifi include, so this stays
// host-testable like WifiReconnectPolicy.
constexpr size_t kMaxWifiSsidBytes = 31;
constexpr size_t kMaxWifiPasswordBytes = 63;

// Whether a submitted (ssid, password) pair is safe to hand to
// wifi_setup.cpp's ApplyWifiCredentials() - checked explicitly here so an
// overlong field is rejected with feedback instead of silently truncated
// by the snprintf() into esp_wifi's fixed-size buffers that would
// otherwise be the only thing standing between the two.
inline bool IsValidWifiCredentials(const std::string& ssid, const std::string& password) {
    return !ssid.empty() && ssid.size() <= kMaxWifiSsidBytes && password.size() <= kMaxWifiPasswordBytes;
}

}  // namespace homedeck
