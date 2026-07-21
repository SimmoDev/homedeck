# Networking

HomeDeck follows a local-first philosophy: LAN communication and direct
device control are preferred over cloud services wherever the integration
allows it, and cloud dependencies are avoided unless genuinely unavoidable
(e.g. currently-unknown constraints in a specific external service's own
architecture). See
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md) for
the tradeoffs and rejected alternatives behind the decisions referenced
below.

## Responsibilities

Networking is a Core service (see
[core.md](core.md#responsibilities)) covering:

- Wi-Fi connection management (associating, reconnecting, credential
  storage via Core's storage service)
- Initial Wi-Fi provisioning (see [Initial Wi-Fi
  provisioning](#initial-wi-fi-provisioning) below)
- Connectivity status, exposed as Core state/events that both the UI (e.g. a
  network-status dashboard widget) and modules (to decide whether to
  attempt a request or fall back to cached data) can consume
- Hosting the embedded HTTP/WebSocket server used by the [Web Management
  UI](web-ui.md) and any module API endpoints
- A thin mDNS/Zeroconf discovery wrapper (see [LAN
  discovery](#lan-discovery) below) — not a universal discovery abstraction,
  just standard mDNS browsing for the modules that actually use it

Modules do not manage their own Wi-Fi or low-level network state — they use
Core's networking service and Core's HTTP server registration mechanism.

## Initial Wi-Fi provisioning

The device has no Wi-Fi credentials out of the box, so it can't simply serve
its own setup UI over the LAN the way the rest of the Web Management UI
does. First-run setup uses a SoftAP: the device broadcasts its own
temporary access point, a phone or laptop connects to it, and a minimal
HTTP setup form (not the full Svelte Web UI — see [web-ui.md](web-ui.md))
collects the Wi-Fi SSID and password, which the device applies via
`esp_wifi_set_config`/`esp_wifi_connect`. Touch UI on-screen keyboard entry
remains available as a fallback for users without a second device handy,
but SoftAP + setup form is the primary, documented path. See
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow)
for why this was chosen over a Touch-UI-first flow or a camera-based
QR-code alternative, and for why the setup form is a small HomeDeck-owned
HTTP handler rather than ESP-IDF's `wifi_provisioning` component (a real
incompatibility with this project's `esp_wifi_remote` stack, not a design
preference).

## LAN discovery

Core provides a thin wrapper around ESP-IDF's `mdns` component for modules
whose external service supports standard mDNS/Zeroconf discovery — Home
Assistant and Kodi both do. This is deliberately not a universal discovery
abstraction: Harmony Hub discovery uses a proprietary UDP broadcast
protocol that doesn't fit an mDNS shape, and Uptime Kuma has no discovery
protocol at all (manual URL entry) — Harmony's discovery logic stays
entirely inside the Harmony module. See
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape)
for why a universal abstraction was rejected.

## Offline behaviour

HomeDeck must degrade gracefully when Wi-Fi or a specific external service
is unavailable. Per CLAUDE.md, this requires:

- Cached configuration, device lists, and dashboard data (via Core's
  storage service — see [ADR-0012](../decisions/ADR-0012-storage-tiers.md)
  for which physical tier this lives on and why), so the device remains
  useful (if read-only) when offline. Cache retention is bounded, not
  unlimited — exact eviction rules are an M2 implementation detail.
- Clear offline indicators, distinguishing live data, cached data, and
  offline state at the UI level (see
  [dashboard.md](dashboard.md#data-freshness))
- Retry with backoff for reconnection attempts, rather than tight polling
  loops that waste battery and flood a recovering service

This is a Core-level contract that modules build on: a module fetching data
from its external service should go through a pattern that naturally
produces "live / cached / offline" state rather than each module inventing
its own retry/caching behavior. Retry/backoff logic itself is a shared Core
utility by default, with modules allowed to layer service-specific
reconnection semantics on top where the external protocol requires it — see
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership)
for why.

## Status

Wi-Fi provisioning and the embedded HTTP server (see [web-ui.md](web-ui.md#status))
are real and confirmed on hardware. Self-advertisement is also real,
**confirmed on hardware** — firmware calls ESP-IDF's `mdns` component
directly (`mdns_init`/`mdns_hostname_set`/`mdns_service_add` in
`firmware/main/homedeck.cpp`) to advertise the device as `homedeck.local`
with an `_http._tcp` service record for the Web UI, once Wi-Fi connects.
Verified from a phone's browser, reaching `http://homedeck.local/`
successfully over the LAN (a desktop Linux client without `nss-mdns`/
Avahi resolution configured won't resolve `.local` names at all — a
client-side gap, not something this device's advertisement can fix). No
Core abstraction wraps this — it's a handful of direct calls with no
simulator-side equivalent needed (the simulator is already reachable at
`localhost`, and desktop OSes run their own mDNS responder for the
machine itself).

Still not implemented: the mDNS *browsing* wrapper this document
describes above, for modules to discover Home Assistant/Kodi. That has no
real consumer until one of those modules exists (M4 for Kodi, M6 for Home
Assistant) — building it now would be exactly the kind of speculative
Core abstraction [ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape)
itself rejects. LAN discovery (browsing) remains planned for whichever of
those milestones lands first.

Connectivity status is also real, **confirmed on hardware**: a portable
`NetworkStatus` interface (`src/platform/network_status.h`) exposes a
`Snapshot()` of connected/SSID/IP state, backed by `FirmwareNetworkStatus`
(updated from `wifi_setup.cpp`'s existing `WIFI_EVENT`/`IP_EVENT`
handlers) and `HostNetworkStatus` for the simulator. A Core-level
`NetworkStatusMonitor` (`src/core/network_status_monitor.h`) polls it on
the existing clock tick and publishes `WifiConnectivityChangedEvent` only
on an actual connected/disconnected transition — the status bar's Wi-Fi
icon is the first consumer. The network-status dashboard widget and the
Web UI's Wi-Fi management page named above are still open, now-unblocked
follow-ups, not built yet.
