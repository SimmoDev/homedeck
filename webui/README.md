# webui/

The Web Management UI frontend — served by the device for administration
(module configuration, diagnostics, logs, OTA, backups, settings). Not the
initial Wi-Fi setup flow, which is a separate SoftAP captive-portal page —
see [docs/architecture/networking.md](../docs/architecture/networking.md#initial-wi-fi-provisioning).

Svelte 5 + TypeScript + Vite (see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)),
plain client-side - no SvelteKit, no routing/SSR need exists for a
single-page admin UI. `src/App.svelte` is currently a scaffold, not real
UI: it fetches `/api/auth/status` and renders the response, proving the
whole pipeline (Vite build → `dist/` → embedded in the firmware app image
or read by the simulator → served → fetched by a real browser). The real
admin/settings/diagnostics screens replace it - see
[docs/architecture/web-ui.md](../docs/architecture/web-ui.md) for the
full scope.

Build:

```sh
npm ci
npm run build   # -> dist/index.html, dist/app.js
npm run check   # svelte-check, also runs in CI
```

`npm run build` is a required, separate step before building firmware or
the simulator - both fail at configure time with a clear error if
`dist/` doesn't exist. See
[DEVELOPMENT.md](../DEVELOPMENT.md#buildtest-workflow) for the full
build order and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage)
for why this isn't auto-invoked from CMake/`idf.py`, and why `vite.config.ts`
forces fixed, non-hashed output filenames (`index.html`/`app.js`)
instead of Vite's default.
