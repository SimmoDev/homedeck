# ADR-0029: Harmony Hub Local Protocol

## Status

Accepted

## Context

[ADR-0003](ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control)
recorded Harmony Hub local control as XMPP-based (port 5222), following
the community's historical reverse-engineering (`pyharmony` and others)
from the era before Logitech's 2018 local-API removal and reinstatement.
[roadmap.md](../roadmap.md)'s M3 section inherited that framing directly:
"Local authentication against an already-paired hub (local XMPP-based
protocol)."

`nmap` against the project's own reference hub (already paired, in active
use) shows only port 8088 open - port 5222 (XMPP) is closed. Live probes
against that same hub during this milestone's implementation confirmed the
hub instead speaks a local WebSocket/JSON API on port 8088, matching more
recent community documentation (`aioharmony`, the Home Assistant
`pyharmony` websockets branch) that XMPP was superseded on modern
firmware, with XMPP now requiring a firmware-side opt-in the reference hub
doesn't have enabled.

## Decision

**Transport: a local WebSocket/JSON API on port 8088, not XMPP.** Two
steps:

1. **Handshake** (plain HTTP, not yet the WebSocket): `POST
   http://<hub-ip>:8088/`, `Content-Type: application/json`, body
   `{"id":<n>,"cmd":"setup.account?getProvisionInfo","timeout":90000}`.
   **Requires an `Origin: http://sl.dhg.myharmony.com` header** - confirmed
   live against the reference hub: omitting it gets a `400` rejection,
   including it succeeds. Not an actual browser origin; just what the
   hub's own validation checks for. The response carries `activeRemoteId`
   (becomes the WebSocket `hubId` below) alongside the owner's Logitech
   account email/username - HomeDeck extracts only `activeRemoteId` and
   never logs or persists the rest.
2. **Control**: WebSocket to
   `ws://<hub-ip>:8088/?domain=svcs.myharmony.com&hubId=<activeRemoteId>`.
   Messages are JSON, shaped
   `{"hubId":..., "timeout":<seconds>, "hbus":{"cmd":<string>, "id":<string>, "params":{...}}}`.
   Config (device/activity list) fetch uses
   `cmd: "vnd.logitech.harmony/vnd.logitech.harmony.engine?config"` -
   confirmed live, returning the reference hub's own device and activity
   list (`src/core/harmony_connection.h`/`.cpp`).

**No authentication of any kind exists in this local path.** The
handshake above needs a specific header, not a credential - anyone on the
LAN can complete it and open the control WebSocket. "Local authentication"
in ADR-0003/roadmap.md's original wording is corrected by this ADR to
"local connection" - there is a handshake, not a login.

**Discovery: manual hub IP/hostname entry, not a broadcast/mDNS
mechanism.** The XMPP-era UDP broadcast discovery protocol
(`_logitech-reverse-bonjour._tcp.local.` on port 5224) some historical
tooling used has no confirmed working equivalent for WebSocket-era
firmware, and this project found no alternative standard mechanism either.
Building a discovery prober against an unconfirmed protocol isn't worth
the cost for the one-hub-per-household case this project's own reference
hardware represents - a Web UI settings field is Harmony's discovery
mechanism, the same shape Uptime Kuma (M5) is already expected to need for
its own manual URL entry.

## Consequences

- Supersedes [ADR-0003](ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control)'s
  Known External Risk section's XMPP framing - that section's "local
  XMPP-based protocol" language describes the historical, no-longer-
  applicable protocol for this project's actual reference hardware.
- [roadmap.md](../roadmap.md)'s M3 "Hub discovery on the LAN" and "Local
  authentication against an already-paired hub (local XMPP-based
  protocol)" items are reworded to reflect manual entry + connection, not
  discovery + authentication.
- [networking.md](../architecture/networking.md#lan-discovery)'s "Harmony
  Hub discovery uses a proprietary UDP broadcast protocol" line is
  corrected - no discovery protocol is used at all.
- New platform capability:
  `WebSocketClient` (`src/platform/websocket_client.h`), backed by
  libcurl's WS API on the simulator and `espressif/esp_websocket_client`
  on firmware - neither target had an outbound WebSocket client before
  this. `HttpClient::Post()` (`src/platform/http_client.h`) also gained an
  `extra_headers` parameter, specifically for the Origin header above.
- The account email/username in the handshake response is personal data
  returned over an unauthenticated local endpoint - a fact about the
  hub's own protocol design, not something HomeDeck's implementation
  introduces; `HarmonyConnection` never reads or stores that field.
- The WebSocket message shapes for activities/devices beyond the
  device/activity `id`/`label` fields HarmonyConnection currently parses
  (`type`, `controlGroup`, per-device command lists, etc.) remain
  community-documented but not independently field-verified - open
  scope for the Devices/Remote control roadmap items still ahead, not
  solved here.
