# webui/

The Web Management UI frontend — served by the device for administration
(module configuration, diagnostics, logs, OTA, backups, settings). Not the
initial Wi-Fi setup flow, which is a separate SoftAP captive-portal page —
see [docs/architecture/networking.md](../docs/architecture/networking.md#initial-wi-fi-provisioning).

Svelte 5 + TypeScript + Vite (see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)),
plain client-side - no SvelteKit, no routing/SSR need exists for a
single-page admin UI. `src/App.svelte` drives the first-login/
session flow - password setup, login, and an authenticated view - off
`GET /api/auth/status`, per ADR-0007's design.
`src/lib/PasswordForm.svelte` is shared by both setup and login (same
field, same submit mechanics; setup adds a confirm-password field, a
safeguard since there's no password-recovery flow yet), with its
validation/error-mapping logic pulled out into
`src/lib/passwordValidation.ts` so it's unit-testable without a
component-testing stack (see `npm run test` below).
Once authenticated, `App.svelte` composes three screens:
`src/lib/Settings.svelte` (device name, weather location search/save,
Harmony hub configuration, backup download/restore - each its own
self-contained sub-component: `DeviceNameSettings.svelte`,
`WeatherSettings.svelte`, `HarmonySettings.svelte`,
`BackupSettings.svelte`), `src/lib/Ota.svelte` (current version, the
battery/power gate's status, upload progress, reboot), and
`src/lib/Diagnostics.svelte` (also a thin composer of three
self-contained sub-components: `CrashDiagnostics.svelte` - reset reason,
downloadable core dump, live battery/power state;
`DiagnosticsLogs.svelte` - a level/component-filterable structured log
view; `WifiReset.svelte` - the Wi-Fi credential reset diagnostic aid).
See [docs/architecture/web-ui.md](../docs/architecture/web-ui.md) for
the full scope, including what's still open (WebSockets for live
updates, module configuration for a second module, Wi-Fi management).

Build:

```sh
npm ci
npm run build   # -> dist/index.html, dist/app.js, dist/app.css
npm run check   # svelte-check, also runs in CI
npm run test    # vitest, also runs in CI
```

`npm run build` is a required, separate step before building firmware or
the simulator - both fail at configure time with a clear error if
`dist/` doesn't exist. See
[DEVELOPMENT.md](../DEVELOPMENT.md#buildtest-workflow) for the full
build order and
[ADR-0025](../docs/decisions/ADR-0025-webui-static-asset-storage.md)
for why this isn't auto-invoked from CMake/`idf.py`, and why `vite.config.ts`
forces fixed, non-hashed output filenames (`index.html`/`app.js`/`app.css`)
instead of Vite's default.
