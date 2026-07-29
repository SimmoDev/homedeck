# ADR-0026: Initial Wi-Fi Provisioning Mechanism

## Status

Accepted. Supersedes the *mechanism* named in
[ADR-0006](ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow)'s
Initial Wi-Fi provisioning decision. That decision's SoftAP +
captive-portal *architecture* (primary path, Touch UI keyboard entry as
fallback) is unchanged and still governs.

## Context

[ADR-0006](ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow)
decided SoftAP + captive portal via ESP-IDF's `wifi_provisioning`
component as the mechanism behind that architecture. `wifi_provisioning`'s
transport implementations (e.g. `wifi_prov_scheme_softap`) only compile
when `CONFIG_ESP_WIFI_ENABLED` or `CONFIG_ESP_HOST_WIFI_ENABLED` is set,
and neither applies to this project's `esp_wifi_remote`/`esp_hosted`
Wi-Fi stack (see [hardware.md#wireless](../architecture/hardware.md#wireless))
— the component has no usable transport on this hardware.

## Decision

Instead of `wifi_provisioning`/`protocomm`, the device runs a minimal,
HomeDeck-owned SoftAP + HTTP server: plain `esp_wifi` AP+STA mode plus
`esp_http_server` serving a small SSID/password form that calls
`esp_wifi_set_config`/`esp_wifi_connect` directly
(`firmware/main/wifi_setup.cpp`).

Two consequences of this mechanism, stated plainly:

- **No protocomm-level encryption of the credential exchange.** The
  original design's payload security (`WIFI_PROV_SECURITY_1`'s X25519 +
  proof-of-possession) came from `protocomm`, which this approach doesn't
  use. The SoftAP itself is unauthenticated (open), matching the common
  consumer-IoT pattern for a brief, physically-local setup window — an
  attacker needs proximity and precise timing, and a leaked Wi-Fi
  password only grants LAN access, not this device. A WPA2-protected AP
  with a per-device password shown on the Tab5's own screen is a natural
  follow-up once the Touch UI has a setup screen to show it on.
- **Credential storage is `esp_wifi`'s own default persistence,
  unencrypted**, on the C6 co-processor's own flash rather than the P4's,
  since `esp_wifi_remote` proxies `esp_wifi_get_config`/
  `esp_wifi_set_config` to the C6 (see
  [hardware.md#wi-fi-bring-up](../architecture/hardware.md#wi-fi-bring-up)).
  Wiring this into Core's Configuration/Storage service instead is a
  documented, low-priority gap, not active scope — see
  [roadmap.md](../roadmap.md)'s Wi-Fi connectivity item.

## Consequences

- [networking.md](../architecture/networking.md) states the resulting
  design without repeating this rationale.
- The Touch UI keyboard fallback (`WifiSetupScreen`) submits through the
  same `ApplyWifiCredentials()` entry point as this HTTP form, so neither
  path reimplements `esp_wifi_set_config`/`connect` independently.
