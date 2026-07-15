# webui/

The Web Management UI frontend — served by the device for administration
(module configuration, diagnostics, logs, OTA, backups, settings). Not the
initial Wi-Fi setup flow, which is a separate SoftAP captive-portal page —
see [docs/architecture/networking.md](../docs/architecture/networking.md#initial-wi-fi-provisioning).

No implementation exists yet. See
[docs/architecture/web-ui.md](../docs/architecture/web-ui.md) for scope,
and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)
for the frontend framework decision (Svelte + Vite). Planned for M2.
