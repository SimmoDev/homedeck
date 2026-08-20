<script lang="ts">
  import { findSetting, loadJson, postJson, type SettingEntry } from "./api";
  import { deviceNameError } from "./deviceNameValidation";

  // Device name (see docs/architecture/web-ui.md#scope and
  // docs/decisions/ADR-0023-settings-backup-api.md) - the mDNS hostname
  // a user can set via the Web UI. Backed by the generic /api/settings
  // endpoint, same as WeatherSettings.svelte.
  const kModuleId = "core";
  const kDeviceNameKey = "device_name";
  const kDeviceNameSchemaVersion = 1;

  let error: string | undefined = $state(undefined);
  let deviceName = $state("");
  let loaded = $state(false);
  let saving = $state(false);
  let saveError: string | undefined = $state(undefined);
  let saved = $state(false);

  async function loadDeviceName() {
    const result = await loadJson<SettingEntry[]>("/api/settings");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    deviceName = findSetting(result.data, kModuleId, kDeviceNameKey) ?? "homedeck";
    loaded = true;
  }

  async function saveDeviceName() {
    // Same double-fired-click guard as BackupSettings.svelte's restore()/
    // WifiReset.svelte's resetWifi() - this button stays mounted with a
    // `disabled` binding rather than unmounting, but a second click
    // event dispatched before Svelte reactively applies that attribute
    // could still reach here before this function's own synchronous
    // `saving = true` below takes effect in the DOM.
    if (saving) return;
    // Trimmed before validating/saving, matching HarmonySettings.svelte's
    // saveHubHost() - a pasted name with incidental leading/trailing
    // whitespace was otherwise rejected outright by deviceNameError()'s
    // charset check instead of silently trimmed.
    const trimmed = deviceName.trim();
    const validationError = deviceNameError(trimmed);
    if (validationError) {
      saveError = validationError;
      saved = false;
      return;
    }
    saving = true;
    saveError = undefined;
    saved = false;
    const result = await postJson("/api/settings", {
      module: kModuleId,
      key: kDeviceNameKey,
      value: trimmed,
      schemaVersion: kDeviceNameSchemaVersion,
    });
    saving = false;
    if (!result.ok) {
      saveError =
        result.kind === "network"
          ? result.message
          : result.body.error === "invalid_value"
            ? "Not a valid device name."
            : `Save failed: ${result.status}`;
      return;
    }
    deviceName = trimmed;
    saved = true;
  }

  loadDeviceName();
</script>

<div class="section">
  <h3>Device name</h3>
  {#if error}
    <p class="error" aria-live="polite">Error: {error}</p>
    <button onclick={loadDeviceName}>Retry</button>
  {:else if !loaded}
    <p class="hint">Loading...</p>
  {:else}
    <div class="row">
      <input
        id="device-name"
        type="text"
        aria-label="Device name"
        bind:value={deviceName}
        disabled={saving}
        maxlength="63"
        oninput={() => {
          saved = false;
          saveError = undefined;
        }}
      />
      <button onclick={saveDeviceName} disabled={saving || deviceName.length === 0}>
        {saving ? "Saving..." : "Save"}
      </button>
    </div>
    <p class="hint">Used as the device's address on your network ({deviceName || "homedeck"}.local).</p>
    {#if saveError}
      <p class="error" aria-live="polite">{saveError}</p>
    {:else if saved}
      <p class="hint" aria-live="polite">Saved.</p>
    {/if}
  {/if}
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
