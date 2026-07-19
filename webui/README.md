# webui/

The Web Management UI frontend — served by the device for administration
(module configuration, diagnostics, logs, OTA, backups, settings). Not the
initial Wi-Fi setup flow, which is a separate SoftAP captive-portal page —
see [docs/architecture/networking.md](../docs/architecture/networking.md#initial-wi-fi-provisioning).

`index.html` is a hand-authored placeholder proving the serving mechanism
end to end (`src/platform/static_assets.h`/`.cpp`) — embedded directly
into the firmware app image on-device, read from this directory at
startup on the simulator; see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage)
for why files here live in the app image rather than a partition. The
real Svelte + Vite frontend (see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach))
replaces it — see [docs/architecture/web-ui.md](../docs/architecture/web-ui.md)
for the full scope. Planned for M2.
