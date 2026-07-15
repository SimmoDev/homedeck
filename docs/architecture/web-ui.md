# Web Management UI

The Web Management UI is a browser-based interface served by the device,
used for administration rather than everyday operation. See
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md#two-interfaces-two-different-design-goals)
for why this is a separate surface from the [Touch UI](ui.md), and
[ADR-0007](../decisions/ADR-0007-web-management-ui-policies.md) for the
tradeoffs and rejected alternatives behind the decisions referenced below.

## Scope

Per CLAUDE.md, the Web UI provides:

- Wi-Fi management (post-provisioning — viewing/changing an already-connected
  network; *not* first-run setup, see [Relationship to Wi-Fi
  provisioning](#relationship-to-wi-fi-provisioning) below)
- Module configuration
- Device status
- Diagnostics
- Logs
- OTA updates
- Backups — Core config and module settings (not cached data), exported/
  restored as a downloadable JSON file, no SD or cloud involvement — see
  [ADR-0012](../decisions/ADR-0012-storage-tiers.md#decision-backup-delivery)
- Settings

Anything that is configuration-heavy, infrequent, or benefits from a full
keyboard/mouse and larger screen belongs here rather than on the Touch UI.

## Transport

The Web UI is served over an embedded HTTP server and communicates with the
device through REST APIs for request/response operations and WebSockets for
live updates (log streaming, live status, diagnostics). Both firmware and
the desktop simulator need to serve the same web assets and expose the same
API/WebSocket contract, though the two run different server implementations
(`esp_http_server` vs. civetweb) behind that shared contract — see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server)
for which server backs each target and the known simulator/hardware
divergence risk that comes with it, including the dispatch-safety
requirement for pushing live updates to a WebSocket from Core's event bus
(not free, and not yet fully pinned down for the simulator's server).

## Diagnostics

The Web UI is the primary surface for the diagnostics CLAUDE.md requires as
a first-class feature — see [diagnostics.md](diagnostics.md) for the full
design (structured logs, module status, connection state, error reporting,
crash/reboot diagnostics). Diagnostics data is produced by Core (see
[core.md](core.md#responsibilities)) and by modules through Core's
services, and rendered here — the Web UI itself has no diagnostic logic of
its own beyond presentation, including for a downloaded core dump, which it
offers as a raw file rather than attempting to decode in the browser (see
[ADR-0013](../decisions/ADR-0013-crash-and-reboot-diagnostics.md)).

## Relationship to Wi-Fi provisioning

Initial Wi-Fi setup is **not** part of this Web Management UI — the device
has no Wi-Fi to serve this UI over until provisioning completes. That first-
run step is a minimal captive-portal page served over a temporary SoftAP,
described in [networking.md](networking.md#initial-wi-fi-provisioning). The
Web Management UI described in this document is what's available once the
device is already on the LAN — module configuration, diagnostics, OTA, and
so on, not first-run bootstrapping.

## Security

See [security.md](security.md) for the cross-cutting picture against all
of CLAUDE.md's security requirements. What's specific to the Web UI:

- The SoftAP captive portal (unauthenticated by nature — there's no
  credential to check yet) is scoped narrowly to collecting Wi-Fi
  credentials, and shuts down once provisioning succeeds. It is not the
  same unauthenticated surface as the full Web Management UI.
- Once on the LAN, configuration-changing endpoints require authentication
  — see [Authentication mechanism](#authentication-mechanism) below for
  what that mechanism is.
- No admin password exists until the first Web UI visit sets one — see
  [Admin password](#admin-password) below.

## Authentication mechanism

A single local admin password, used for session/cookie-based login. See
[ADR-0007](../decisions/ADR-0007-web-management-ui-policies.md#decision-authentication-mechanism)
for why this was chosen over token-based auth.

## Admin password

The SoftAP captive portal collects Wi-Fi credentials only — it does not
also collect the admin password. The device joins the LAN with no admin
password set, and the first time anyone opens the Web UI, they're required
to set one before anything else is accessible, the same first-login pattern
routers and NAS devices use. Until that happens, the Web UI's only
reachable state is the password-setup screen. See
[ADR-0007](../decisions/ADR-0007-web-management-ui-policies.md#decision-when-the-admin-password-is-set)
for why this wasn't folded into the SoftAP flow instead, and that ADR's
"Accepted risk" note for the race condition this pattern has on a shared
or compromised network.

## Status

Not yet implemented. The frontend is Svelte + Vite — see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach).
Planned for M2.
