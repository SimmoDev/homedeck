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
temporary access point, a computer or phone connects to it, and a minimal
HTTP setup form (not the full Svelte Web UI — see [web-ui.md](web-ui.md))
collects the Wi-Fi SSID and password, which the device applies via
`esp_wifi_set_config`/`esp_wifi_connect`. The form's submitted values are
percent-decoded and length-validated before being applied
(`src/core/url_codec.h`, `src/core/wifi_credentials.h`) so a network name
or password containing a space or symbol is handled correctly rather than
corrupted. Touch UI on-screen keyboard entry
remains available as a fallback for users without a second device handy,
but SoftAP + setup form is the primary, documented path. See
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow)
for why this was chosen over a Touch-UI-first flow or a camera-based
QR-code alternative, and
[ADR-0026](../decisions/ADR-0026-wifi-provisioning-mechanism.md) for why
the setup form is a small HomeDeck-owned HTTP handler rather than
ESP-IDF's `wifi_provisioning` component (a real incompatibility with
this project's `esp_wifi_remote` stack, not a design preference).

## LAN discovery

Core provides a thin wrapper around ESP-IDF's `mdns` component for modules
whose external service supports standard mDNS/Zeroconf discovery — Home
Assistant and Kodi both do. This is deliberately not a universal discovery
abstraction: Harmony Hub has no confirmed working discovery protocol at
all for current-generation firmware (a manually-entered hub address is
its own module's mechanism instead — see
[ADR-0029](../decisions/ADR-0029-harmony-local-protocol.md)), and Uptime
Kuma has no discovery protocol either (manual URL entry). See
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape)
for why a universal abstraction was rejected.

## Offline behaviour

HomeDeck must degrade gracefully when Wi-Fi or a specific external service
is unavailable. Per [CLAUDE.md](../../CLAUDE.md), this requires:

- Cached configuration, device lists, and dashboard data (via Core's
  storage service — see [ADR-0012](../decisions/ADR-0012-storage-tiers.md)
  for which physical tier this lives on and why), so the device remains
  useful (if read-only) when offline. Cache retention is bounded, not
  unlimited — exact eviction rules are an M2 implementation detail.
- Clear offline indicators, distinguishing live data, cached data, and
  offline state at the UI level (see
  [dashboard.md](dashboard.md#data-freshness))
- Retry with backoff for reconnection attempts, rather than tight polling
  loops that waste battery and flood a recovering service. This is the
  module-level contract described below, distinct from `wifi_setup.cpp`'s
  own radio-level reconnect (a fixed retry interval, not exponential
  backoff — see its own comments for why that's an accepted simplification
  at this layer).

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
are implemented. Self-advertisement is also implemented — firmware calls
ESP-IDF's `mdns` component directly (`mdns_init`/`mdns_hostname_set`/
`mdns_service_add` in `firmware/main/homedeck.cpp`) to advertise the
device as `homedeck.local` with an `_http._tcp` service record for the
Web UI, once Wi-Fi connects (a desktop Linux client without `nss-mdns`/
Avahi resolution configured won't resolve `.local` names at all — a
client-side gap, not something this device's advertisement can fix). No
Core abstraction wraps this — it's a handful of direct calls with no
simulator-side equivalent needed (the simulator is already reachable at
`localhost`, and desktop OSes run their own mDNS responder for the
machine itself).

The mDNS *browsing* wrapper is also implemented, as of M4 (Kodi is its
first consumer — the point at which it stopped being the speculative
Core abstraction
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape)
rejected building ahead of one). A portable `MdnsBrowser` interface
(`src/platform/mdns_browser.h`) exposes a single
`Browse(service_type, timeout)` that returns **every** resolved
instance — never a single "the" result, since multiple instances of one
service type on a LAN (a media box per room) is expected, and choosing
between them is the calling module's policy. It is backed by
`FirmwareMdnsBrowser` (ESP-IDF's `mdns` component, `mdns_query_ptr`) and
`HostMdnsBrowser` (libavahi-client, simulator only — see
[DEVELOPMENT.md](../../DEVELOPMENT.md); returns empty rather than failing
when no local mDNS responder is running). Home Assistant (M6) is the
expected second consumer.

Test coverage sits at the caller's selection policy, not the backend
adapters: `KodiClient`'s discovery/selection tests
(`tests/kodi_client_test.cpp`) drive a fake `MdnsBrowser`, and neither
`HostMdnsBrowser` nor `FirmwareMdnsBrowser` has a unit test of its own
(a hermetic loopback mDNS responder is impractical to stand up the way
`websocket_client_test.cpp` stands up a loopback WebSocket server).
`FirmwareMdnsBrowser` also has no automated firmware target, so its
`mdns_query_ptr` result walk is exercised only on-device — folded into
the on-hardware validation M4 (Media) needs before release, not the M4a
part.

Connectivity status is also implemented: a portable `NetworkStatus`
interface (`src/platform/network_status.h`) exposes a `Snapshot()` of
connected/SSID/IP state, backed by `FirmwareNetworkStatus` (updated from
`wifi_setup.cpp`'s existing `WIFI_EVENT`/`IP_EVENT` handlers) and
`HostNetworkStatus` for the simulator. A Core-level
`NetworkStatusMonitor` (`src/core/network_status_monitor.h`) polls it on
the existing clock tick and publishes `WifiConnectivityChangedEvent` only
on an actual connected/disconnected transition — the status bar's Wi-Fi
icon is the first consumer, and the network-status dashboard widget
(`NetworkStatusWidget`, see [dashboard.md](dashboard.md#status)) is the
second. A Web UI Wi-Fi management page (viewing/changing stored
credentials post-provisioning, see [web-ui.md](web-ui.md#status) and the
M7 item in [roadmap.md](../roadmap.md)) is a now-unblocked follow-up, not
built yet.

Outbound HTTP(S) is also implemented — a portable `HttpClient` interface
(`src/platform/http_client.h`, `Get()`/`Post()`) backed by
`FirmwareHttpClient` (`esp_http_client`, TLS verified via ESP-IDF's
built-in certificate bundle rather than a pinned cert) and
`HostHttpClient` (libcurl) for the simulator. The first consumer was the
weather widget's Open-Meteo integration (GET-only, see
[dashboard.md](dashboard.md#status)); `Post()` (with an optional
extra-headers list) was added for Harmony's hub handshake (see
[ADR-0029](../decisions/ADR-0029-harmony-local-protocol.md)). The same
interface is expected to back Uptime Kuma's and Home Assistant's own
outbound calls once those modules exist (M5-M6), not a single-purpose
addition. Kodi (M4a) is the exception among the near-term modules: it is
WebSocket-only and uses no `HttpClient` at all — its `image://` artwork
URLs would need Kodi's authenticated HTTP port, deferred to M7 (see
[ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)).

A portable outbound `WebSocketClient` interface
(`src/platform/websocket_client.h`) is also implemented — text-frame
only, a blocking connect/send/receive shape a caller's own background
`Task` drives directly. `HostWebSocketClient` backs it on the simulator
via libcurl's `CURLOPT_CONNECT_ONLY` plus a hand-rolled RFC 6455
handshake/framing over `curl_easy_send()`/`curl_easy_recv()`, not
libcurl's own native `curl_ws_send()`/`curl_ws_recv()` API — see
ADR-0029's Consequences section for why. `FirmwareWebSocketClient`
bridges `espressif/esp_websocket_client`'s event-callback API to the same
blocking shape on firmware. `HarmonyConnection`
(`src/core/harmony_connection.h`) was the first consumer (see ADR-0029);
`KodiClient` (`src/core/kodi_client.h`, M4a) is the second, reusing it
unchanged for Kodi's JSON-RPC WebSocket (see
[ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)).
