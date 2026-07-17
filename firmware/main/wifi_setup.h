#pragma once

namespace homedeck {

// Brings the device onto Wi-Fi. If credentials are already stored (via
// esp_wifi's own default persistence - the C6 co-processor's own flash
// under this project's esp_wifi_remote stack, not the P4's), connects
// directly; otherwise starts a temporary open SoftAP ("HomeDeck-xxxxxx")
// and a minimal HTTP setup form at its gateway address, and blocks until
// a phone or laptop submits Wi-Fi credentials through it and the device
// connects. See docs/architecture/networking.md#initial-wi-fi-provisioning
// and ADR-0006 for why this is a small HomeDeck-owned HTTP form rather
// than ESP-IDF's wifi_provisioning component (a real incompatibility with
// this project's esp_wifi_remote stack, not a design preference).
//
// Must be called after esp_netif_init() and
// esp_event_loop_create_default().
void ConnectToWifi();

}  // namespace homedeck
