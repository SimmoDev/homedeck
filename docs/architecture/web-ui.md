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

The `HttpServer` primitive is implemented and real on both targets —
`FirmwareHttpServer` (`esp_http_server`) and `HostHttpServer` (civetweb),
each serving one real route (`GET /`, a plain-text placeholder body) on
their target's default port. On firmware, it starts after
`ConnectToWifi()` returns, once `wifi_setup.cpp`'s own temporary SoftAP
server has already stopped. **Confirmed on real hardware** (Tab5 K145
reference unit) — the log shows `Web UI listening on port 80` right
after Wi-Fi connects, and the route is reachable over the LAN from a
browser. Also confirmed with a real request/response round trip in an
automated test (a raw-socket HTTP client against `HostHttpServer` in
`tests/http_server_test.cpp`) and manually against the running
simulator.

**The [authentication mechanism](#authentication-mechanism) is real** —
`AdminAuthService` (`src/core/admin_auth_service.h`/`.cpp`) implements
ADR-0007's single-admin-password/session-login design and its
first-login-sets-the-password flow: `GET /api/auth/status`,
`POST /api/auth/setup`, `POST /api/auth/login`, and
`POST /api/auth/logout`, plus a `RequireAuth()` wrapper other endpoints
will use once they exist. Passwords are PBKDF2-SHA256 hashed (salted,
25,000 iterations - see `admin_auth_service.cpp`'s own comment for the
latency/brute-force-resistance rationale behind that specific count)
via mbedtls, the same library on both targets
(vendored as a single header for the host build, ESP-IDF's own copy on
firmware — see [src/README.md](../../src/README.md)); mbedtls also
supplies the CSPRNG for salts and session tokens, so no separate
random-source abstraction was needed. Sessions are an in-memory table
(not persisted - a reboot requires re-login) with a 24-hour lifetime,
guarded by a mutex since HTTP server worker threads can call into this
service concurrently, unlike most of Core, which only the LVGL UI task
touches. Session expiry uses a monotonic clock
(`SteadyTimeSource`, `src/platform/steady_time_source.h`, wrapping
`std::chrono::steady_clock`) on both targets rather than the wall-clock
`TimeSource` — the correct mechanism for expiry comparisons regardless
of target, and specifically necessary on firmware, where the wall-clock
`Rx8130TimeSource` reads the RTC over I2C on every call and the RTC has
a known, pre-existing never-calibrated gap (see
[ADR-0016](../decisions/ADR-0016-battery-rtc-library.md)).

**Confirmed on real hardware** (Tab5 K145 reference unit), as well as
end to end on the simulator (an automated raw-socket test,
`tests/admin_auth_routes_test.cpp`, drives the actual setup → protected
route → login → wrong-password → logout sequence over real HTTP against
`HostHttpServer`) and via a clean Docker rebuild with host tests
passing — `status`, `login` against a password that survives a reboot
(it's in NVS), and the protected route all behave correctly on the
Tab5. `esp_http_server`'s task stack is raised to 8KB
(`FirmwareHttpServer::Start()`, `src/platform/firmware/http_server.cpp`)
from its 4KB default, and ESP-IDF's hardware-accelerated SHA256 is
disabled project-wide (`CONFIG_MBEDTLS_HARDWARE_SHA=n` in
`firmware/sdkconfig.defaults`) in favor of software SHA256, which has no
per-call DMA overhead.

**NVS encryption is a known, deliberately separated gap, not an
oversight** — the admin password hash is stored in the existing plain
NVS tier, not (yet) the HMAC-peripheral-encrypted scheme ADR-0010
requires; see that ADR's own implementation note for why enabling
encryption stayed a separate follow-up rather than landing in the same
pass as new, not-yet-hardware-verified auth logic. The password itself
is still never stored reversibly regardless (PBKDF2-SHA256 hashed before
it reaches Storage).

**Static asset serving is real on both targets** — `ServeStaticFiles`
(`src/platform/static_assets.h`/`.cpp`) registers one exact-path GET
handler per asset. Firmware embeds assets directly into the app image
via ESP-IDF's `EMBED_FILES` (see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage)
for why, not the `storage` FAT partition); the simulator reads the same
built files from `webui/dist/` on disk once at startup — **confirmed on
real hardware** (Tab5 K145 reference unit), reachable over the LAN at
`http://homedeck.local/` once Wi-Fi connects, as well as via a real HTTP
request against the simulator and a clean Docker firmware build with the
embedded symbols linking correctly.

**The Svelte + Vite frontend is real, and the first-login/session flow
is now real UI, not scaffolding** (`webui/`, see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)
for the framework decision). `App.svelte` drives three states directly
off `GET /api/auth/status` — password setup (`PasswordForm.svelte` in
`setup` mode, with a confirm-password field since there's no recovery
path for a setup typo yet), login (the same component in `login` mode,
single field), and an authenticated view with a working logout button —
matching ADR-0007's first-login-sets-password design exactly, including
its accepted race-condition handling (an `already_set` response from a
losing setup request re-checks status rather than treating it as this
form's own error). Confirmed on both targets: the setup and login form
renders confirmed via headless Chromium against the real running
server (correct fields, `autocomplete` attributes, button text per
mode); the full session lifecycle (wrong password → 401, correct
password → session cookie → `authenticated: true`, logout → cookie
cleared → `authenticated: false`) confirmed against the actual
`AdminAuthService` endpoints on the simulator, matching exactly what
the form code depends on. **Confirmed on real hardware** (Tab5 K145
reference unit) too — the full three-file bundle
(`index.html`/`app.js`/`app.css`) served correctly over the LAN at
`http://homedeck.local/`, and the wrong-password path specifically
(401/`invalid_credentials`, the same response `PasswordForm.svelte`
maps to "Incorrect password."). Basic layout/spacing styling exists
(Svelte component-scoped `<style>` blocks plus a small global reset in
`index.html`) — a plain, functional look for the real screens that
exist, not a design system built ahead of having more screens to
standardize across.

**The diagnostics screen is real too** — see
[diagnostics.md#status](diagnostics.md#status) for the full detail
(crash/reboot reset reason and a downloadable core dump; the only
diagnostic data that exists yet).

**OTA update support is real, confirmed on hardware** — see
[hardware.md#power](hardware.md#power) for the battery/external-power
gate it's built on. `GET /api/ota/status`, `POST /api/ota/upload`, and
`POST /api/ota/reboot` (`src/core/ota_routes.h`/`.cpp`), gated by
`EvaluateOtaGate()` (`src/core/ota_gate.h`) per
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate),
all admin-only via `RequireAuth()`. `webui/src/lib/Ota.svelte` shows
current version, the gate's reason if closed, an upload with real
progress (`XMLHttpRequest`, not `fetch` - see the component's own
comment for why), and an explicit "Reboot now" step, not an automatic
reboot after upload. Confirmed end to end on the K145 reference unit: a
real multi-MB image uploaded and booted into over the LAN, and
app-rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) automatically
reverting to the previous slot after a deliberately-bad image was
uploaded and rebooted into.

**Settings and backups are also real** - `GET`/`POST /api/settings`,
`POST /api/settings/erase`, `GET /api/backup`, `POST
/api/backup/restore` (`src/core/settings_routes.h`/`.cpp`), all
admin-only, built directly on `Storage::SetSetting/GetSetting/EraseSetting`
and a new `Storage::ListAllSettings()` (see
[ADR-0023](../decisions/ADR-0023-settings-backup-api.md) for the
enumeration mechanism and a real security finding it addresses: on
firmware, `SettingsStore` and `SecretStore` share the same physical NVS
namespace, so the admin password hash needs an explicit reserved-key
guard to stay out of this generic surface - confirmed via a dedicated
regression test). `webui/src/lib/Settings.svelte` shows a concrete
device name field (the first real setting - replaces the previously
hardcoded `"homedeck"` mDNS hostname, applied live via
`mdns_hostname_set()` without a reboot) plus backup download/restore -
deliberately not a generic raw settings editor, since device name alone
gave the design nothing to check itself against. Confirmed
end to end against both the simulator and the K145 reference unit
(`curl` against a real running server, including the
admin-password-exclusion regression scenario and the live mDNS
re-announce, with no reboot).

**Weather location is the second real setting** - a place-name search
box backed by a new `GET /api/weather/geocode?query=<name>` endpoint
(`src/core/weather_routes.h`/`.cpp`, admin-only like every other
endpoint here), proxying Open-Meteo's own free geocoding API rather
than the browser calling it directly, so the Web UI stays a client of
only the device's own API surface. Selecting a search result saves
`latitude`/`longitude`/`display_name` through the existing generic
settings endpoints above - no dedicated save endpoint needed, confirming
the generic API's design holds up against a second real consumer - then
calls a second new endpoint, `POST /api/weather/refresh`, which wakes
`OpenMeteoWeatherProvider`'s poll loop immediately (see
[dashboard.md](dashboard.md#status)) rather than leaving the dashboard
to wait out the rest of a real 30-minute poll interval before showing
anything for the location just chosen. Confirmed end to end (search,
select, save, refresh, and the dashboard's weather widget picking up
the change within a couple of seconds) against the simulator, a real
desktop browser session, and the K145 reference unit.

Still open, each its own future pass: WebSockets for live updates,
module configuration specifically (no real module exists yet to
configure - the generic settings API above is ready for one), Wi-Fi
management (view/change post-provisioning), factory-reset, and the
NVS-encryption follow-up named above.
