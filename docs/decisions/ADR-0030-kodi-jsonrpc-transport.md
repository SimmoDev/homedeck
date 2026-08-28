# ADR-0030: Kodi JSON-RPC Transport

## Status

Accepted.

## Context

[roadmap.md](../roadmap.md)'s M4 makes Kodi HomeDeck's second
integration, the first Media module. Kodi exposes one JSON-RPC API over
three transports:

| Transport | Port | Auth | Server push |
|---|---|---|---|
| HTTP | 8080 | HTTP Basic (username + password) | none — request/response only |
| Raw TCP | 9090 | none | yes — unsolicited notifications |
| WebSocket | 9090, path `/jsonrpc` | none | yes — same notification stream |

Verified against a reference Kodi 21 (Omega) instance on Android TV: the
9090 WebSocket accepts an unauthenticated upgrade and carries the full
API (player state, library, playback and input control); the 8080 HTTP
endpoint returns `401` without a credential.

Two properties of a deployed setup shape the decision:

- **Multiple Kodi instances on one LAN is normal** — a box per room —
  unlike Harmony's one-hub-per-household assumption
  ([ADR-0029](ADR-0029-harmony-local-protocol.md)).
- **On Android/Google TV, Kodi is an app that is frequently not
  running.** Being unreachable is its resting state, not a fault — see
  [kodi.md](../architecture/kodi.md).

## Decision

### Transport: unauthenticated WebSocket JSON-RPC on port 9090

`KodiClient` connects to `ws://<host>:9090/jsonrpc` over the existing
`WebSocketClient` platform capability
(`src/platform/websocket_client.h`), built for Harmony in M3 — no new
platform code.

**Rejected — HTTP on 8080.** It needs a stored username + password,
which would be HomeDeck's first module credential and would pull
[ADR-0018](ADR-0018-staged-security-hardening.md)'s Standard-tier
NVS-encryption trigger forward from M6 to M4. It also has no push
channel, so Now Playing would poll. No compensating benefit.

**Rejected — raw TCP on 9090.** Same auth and push properties as the
WebSocket, but needs a new raw-socket platform capability on both
targets; the WebSocket reuses what exists.

### Server push, and the module-internal shape it forces

The 9090 API pushes JSON-RPC notifications (player state, volume)
unsolicited to every connected client, with no subscribe call. Now
Playing is therefore event-driven, not the "up to 30s stale" polling
`HarmonyConnection` is limited to — this is the main reason for the
transport choice.

The consequences are internal to the module, not Core: the receive loop
correlates responses to calls by JSON-RPC `id` while applying
interleaved notifications to a mutex-guarded snapshot, and a periodic
reconcile-poll backstops any missed notification. Wire-level detail
(notification payloads, player-id resolution, field constraints) is
documented in [kodi.md](../architecture/kodi.md), not here.

### Discovery: mDNS browse for `_xbmc-jsonrpc._tcp`

Core gains the thin mDNS **browsing** wrapper deferred from M2's LAN
discovery item
([ADR-0006](ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape),
[networking.md](../architecture/networking.md#lan-discovery)) — Kodi is
its first consumer, Home Assistant (M6) the second. `Browse()` returns a
**list**, never a single result, because multiple instances is the
normal case.

### Instance selection: keyed by the mDNS TXT `uuid`

The chosen instance is persisted by the `uuid` from its mDNS TXT record
(module `kodi`, key `instance_uuid`), not its host or IP — the `uuid`
survives DHCP changes and Kodi restarts. Each connection attempt
re-resolves `uuid` → current address via a browse.

- A manual **host override** (key `host`) pins an address and skips
  browse resolution, for a Kodi that isn't discoverable — the role
  Harmony's `hub_host` plays.
- One instance discovered and nothing saved → auto-select it. Two or
  more → the UI asks the user to choose rather than guessing.
- A saved-but-offline instance does **not** fall back to another
  discovered instance; silently controlling a different room is worse
  than doing nothing.

### "Unreachable" raises no notification

A failed or dropped Kodi connection publishes no `NotificationEvent` —
there is no `HarmonyNotificationBridge` equivalent. The widget and
screens show a plain "not reachable" indicator and `RetryBackoff`
continues quietly. Given Kodi's normal resting state on Android/Google
TV (Context), an outage notification would be constant noise.

### Artwork is out of M4 scope

The `image://…` artwork URLs Kodi returns resolve only through its HTTP
image endpoint on the authenticated 8080 port. Now Playing is text +
progress bar for M4; artwork is revisited in M7 alongside the
credential / NVS-encryption question.

## Consequences

- The **mDNS browsing wrapper is built now** (`src/platform/mdns_browser.h`
  + firmware and host backends). This is no longer the speculative Core
  abstraction
  [ADR-0006](ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape)
  rejected — it has a concrete consumer. The host backend's approach
  (an Avahi client library vs. a hand-rolled mDNS query) is an
  implementation choice; a new runtime dependency, if taken, gets a
  reciprocal note on ADR-0006.
- **`KodiClient` is the second `Module` implementation**
  ([ADR-0003](ADR-0003-module-architecture.md)) and its test: the
  `id`-correlation receive loop and the request/response hand-off for
  UI-thread browse calls both sit inside the module — no Core change is
  needed to fit the Harmony-derived contract.
- **No personal data crosses this path**, unlike Harmony's handshake
  (ADR-0029). Kodi's 9090 API does expose full library and control to
  any LAN client with no auth — a property of Kodi's design, within the
  local-first threat model already accepted for Harmony.
- **Library-browse response shapes are only partially verified** (M4b
  scope); anything `KodiClient` parses beyond the verified set is
  flagged in [kodi.md](../architecture/kodi.md), the caveat ADR-0029
  also carries.
