<script lang="ts">
  import { findSetting, loadJson, postJson, type SettingEntry } from "./api";
  import { hubHostError } from "./harmonyValidation";
  import { tripGuard } from "./guardedAction";

  // Harmony Hub module configuration (see docs/roadmap.md's M3 section
  // and docs/decisions/ADR-0029-harmony-local-protocol.md) - just a hub
  // address, no credential field: this protocol has no authentication
  // step at all. Backed by the generic /api/settings endpoint, same as
  // WeatherSettings.svelte/DeviceNameSettings.svelte, plus
  // core/harmony_routes.h's two Harmony-specific endpoints for status/
  // reconnect.
  const kModuleId = "harmony";
  const kHubHostKey = "hub_host";
  const kSchemaVersion = 1;

  interface HarmonyDevice {
    id: string;
    label: string;
  }

  interface HarmonyActivity {
    id: string;
    label: string;
  }

  interface HarmonyStatus {
    state: "disconnected" | "connecting" | "connected" | "error";
    hasConfig: boolean;
    devices: HarmonyDevice[];
    activities: HarmonyActivity[];
  }

  let error: string | undefined = $state(undefined);
  let loaded = $state(false);
  let hubHost = $state("");
  let saving = $state(false);
  let saveError: string | undefined = $state(undefined);
  let saved = $state(false);

  let status: HarmonyStatus | undefined = $state(undefined);
  let statusError: string | undefined = $state(undefined);
  let statusLoading = $state(false);

  async function loadHubHost() {
    const result = await loadJson<SettingEntry[]>("/api/settings");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    hubHost = findSetting(result.data, kModuleId, kHubHostKey) ?? "";
    loaded = true;
  }

  async function loadStatus() {
    statusLoading = true;
    const result = await loadJson<HarmonyStatus>("/api/harmony/status");
    statusLoading = false;
    if (result.error !== undefined) {
      statusError = result.error;
      return;
    }
    statusError = undefined;
    status = result.data;
  }

  async function saveHubHost() {
    const trimmed = hubHost.trim();
    const validationError = hubHostError(trimmed);
    if (validationError) {
      saveError = validationError;
      saved = false;
      return;
    }
    if (tripGuard(() => saving, () => (saving = true))) return;
    saveError = undefined;
    saved = false;
    const result = await postJson("/api/settings", {
      module: kModuleId,
      key: kHubHostKey,
      value: trimmed,
      schemaVersion: kSchemaVersion,
    });
    saving = false;
    if (!result.ok) {
      saveError =
        result.kind === "network"
          ? result.message
          : result.body.error === "invalid_value"
            ? "Not a valid hub address."
            : result.body.error === "value_too_long"
              ? "Hub address is too long."
              : `Save failed: ${result.status}`;
      return;
    }
    hubHost = trimmed;
    saved = true;
    // Clears the old hub's numbers immediately rather than leaving them
    // on screen until the status GET below resolves - status.state===
    // "connected" is the only branch that renders device/activity
    // counts (see the template below), so this alone is enough to hide
    // them without needing a synthetic placeholder state.
    status = undefined;
    // Awaited, not fire-and-forget, so the status GET below can't reach
    // the backend before this has - wakes HarmonyConnection's own
    // connect loop immediately rather than leaving it to notice the new
    // address on its own retry schedule. postJson() (not a bare fetch())
    // so a lapsed session still routes through notifyIfSessionExpired()
    // like every other request in this app. Still only a best-effort
    // ordering, not a guarantee the connect loop has acted on the
    // trigger by the time the status GET below actually runs - it's a
    // background Task on its own thread (src/core/harmony_connection.h),
    // not something this request waits on synchronously.
    await postJson("/api/harmony/reconnect");
    // The Refresh button below covers whatever this snapshot doesn't
    // catch, since no live-push mechanism exists yet for the Web UI (see
    // docs/decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server).
    loadStatus();
  }

  function stateLabel(state: HarmonyStatus["state"]): string {
    switch (state) {
      case "disconnected":
        return "Not connected";
      case "connecting":
        return "Connecting...";
      case "connected":
        return "Connected";
      case "error":
        return "Connection error - retrying";
    }
  }

  loadHubHost();
  loadStatus();
</script>

<div class="section">
  <h3>Harmony Hub</h3>
  {#if error}
    <p class="error" aria-live="polite">Error: {error}</p>
    <button onclick={loadHubHost}>Retry</button>
  {:else if !loaded}
    <p class="hint">Loading...</p>
  {:else}
    <div class="row">
      <!-- maxlength is well under the server's generic
           kMaxSettingValueLength (4096, settings_routes.cpp) -
           saveHubHost()'s "value_too_long" branch below is
           defense-in-depth against a bypassed maxlength (devtools, a
           scripted POST), not something this input can trigger on its
           own. -->
      <input
        id="harmony-hub-host"
        type="text"
        aria-label="Harmony Hub address"
        placeholder="Hub IP address or hostname"
        maxlength="255"
        bind:value={hubHost}
        disabled={saving}
        oninput={() => {
          saved = false;
          saveError = undefined;
        }}
      />
      <button onclick={saveHubHost} disabled={saving}>
        {saving ? "Saving..." : "Save"}
      </button>
    </div>
    <p class="hint">
      No password or account needed - already-paired Harmony Hubs have no authentication step on the local
      network. Save with the field empty to disconnect and clear the configured hub.
    </p>
    {#if saveError}
      <p class="error" aria-live="polite">{saveError}</p>
    {:else if saved}
      <p class="hint" aria-live="polite">Saved.</p>
    {/if}
  {/if}

  <!-- Outside the error/loaded gate above on purpose - loadHubHost() and
       loadStatus() run independently (see their own calls below), so a
       transient loadHubHost() failure must not hide a status fetch that
       succeeded on its own; nothing here depends on hubHost/loaded. -->
  <div class="row status-row">
    {#if statusError}
      <p class="error" aria-live="polite">Status error: {statusError}</p>
    {:else if status}
      <p>
        Status: <strong>{stateLabel(status.state)}</strong>
        {#if status.hasConfig}
          &mdash; {status.devices.length} device{status.devices.length === 1 ? "" : "s"}, {status.activities.length}
          activit{status.activities.length === 1 ? "y" : "ies"}
        {/if}
      </p>
    {/if}
    <button onclick={loadStatus} disabled={statusLoading}>
      {statusLoading ? "Refreshing..." : "Refresh"}
    </button>
  </div>
</div>

<style>
  .section {
    text-align: left;
    margin-bottom: 1.25rem;
  }

  h3 {
    margin: 0 0 0.5rem;
    font-size: 1rem;
  }

  p {
    margin: 0 0 0.5rem;
  }

  .row {
    display: flex;
    gap: 0.5rem;
    align-items: center;
    margin-bottom: 0.5rem;
  }

  .status-row {
    justify-content: space-between;
  }

  .status-row p {
    margin: 0;
  }

  input[type="text"] {
    flex: 1;
    padding: 0.4rem 0.5rem;
    border: 1px solid #d1d5db;
    border-radius: 0.375rem;
  }

  .hint {
    color: #4b5563;
    font-size: 0.875rem;
  }

  .error {
    color: #b91c1c;
    font-size: 0.875rem;
  }
</style>
