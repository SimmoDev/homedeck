<script lang="ts">
  // Settings/backup (see docs/architecture/web-ui.md#scope and
  // docs/decisions/ADR-0023-settings-backup-api.md). The backend API is
  // generic (any module/key), but this shows only what's real today -
  // there's exactly one user-facing setting (device name) until a real
  // module exists; a generic raw key-value editor would have no second
  // data point to design against, and the backup download already gives
  // a complete "see everything" escape hatch for debugging.
  interface SettingEntry {
    module: string;
    key: string;
    value: string;
    schemaVersion: number;
  }

  const kModuleId = "core";
  const kDeviceNameKey = "device_name";
  const kDeviceNameSchemaVersion = 1;

  let error: string | undefined = $state(undefined);
  let deviceName = $state("");
  let deviceNameLoaded = $state(false);
  let savingDeviceName = $state(false);
  let deviceNameError: string | undefined = $state(undefined);
  let deviceNameSaved = $state(false);

  let restoreFile: File | undefined = $state(undefined);
  let restoring = $state(false);
  let restoreResult: string | undefined = $state(undefined);
  let restoreError: string | undefined = $state(undefined);

  async function loadSettings() {
    try {
      const response = await fetch("/api/settings");
      if (!response.ok) {
        error = `Request failed: ${response.status}`;
        return;
      }
      error = undefined;
      const entries = (await response.json()) as SettingEntry[];
      const current = entries.find((entry) => entry.module === kModuleId && entry.key === kDeviceNameKey);
      deviceName = current?.value ?? "homedeck";
      deviceNameLoaded = true;
    } catch (err) {
      error = String(err);
    }
  }

  async function saveDeviceName() {
    savingDeviceName = true;
    deviceNameError = undefined;
    deviceNameSaved = false;
    try {
      const response = await fetch("/api/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          module: kModuleId,
          key: kDeviceNameKey,
          value: deviceName,
          schemaVersion: kDeviceNameSchemaVersion,
        }),
      });
      if (!response.ok) {
        const body = await response.json().catch(() => ({}));
        deviceNameError = body.error === "invalid_value" ? "Not a valid device name." : `Save failed: ${response.status}`;
        return;
      }
      deviceNameSaved = true;
    } catch (err) {
      deviceNameError = String(err);
    } finally {
      savingDeviceName = false;
    }
  }

  function onRestoreFileChange(event: Event) {
    const input = event.target as HTMLInputElement;
    restoreFile = input.files?.[0];
    restoreResult = undefined;
    restoreError = undefined;
  }

  async function restore() {
    if (!restoreFile) return;
    restoring = true;
    restoreResult = undefined;
    restoreError = undefined;
    try {
      const body = await restoreFile.text();
      const response = await fetch("/api/backup/restore", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body,
      });
      if (!response.ok) {
        restoreError = `Restore failed: ${response.status}`;
        return;
      }
      const result = (await response.json()) as { applied: number; failed: unknown[] };
      restoreResult = `Restored ${result.applied} setting${result.applied === 1 ? "" : "s"}${
        result.failed.length > 0 ? `, ${result.failed.length} failed` : ""
      }.`;
      await loadSettings();
    } catch (err) {
      restoreError = String(err);
    } finally {
      restoring = false;
    }
  }

  loadSettings();
</script>

<div class="settings">
  <h2>Settings</h2>
  {#if error}
    <p class="error">Error: {error}</p>
  {:else if !deviceNameLoaded}
    <p class="hint">Loading...</p>
  {:else}
    <label for="device-name">Device name</label>
    <div class="row">
      <input
        id="device-name"
        type="text"
        bind:value={deviceName}
        disabled={savingDeviceName}
        oninput={() => (deviceNameSaved = false)}
      />
      <button onclick={saveDeviceName} disabled={savingDeviceName || deviceName.length === 0}>
        {savingDeviceName ? "Saving..." : "Save"}
      </button>
    </div>
    <p class="hint">Used as the device's address on your network ({deviceName || "homedeck"}.local).</p>
    {#if deviceNameError}
      <p class="error">{deviceNameError}</p>
    {:else if deviceNameSaved}
      <p class="hint">Saved.</p>
    {/if}

    <h3>Backup</h3>
    <p class="hint">Download a copy of your settings, or restore from a previously downloaded file.</p>
    <a class="button-link" href="/api/backup" download="homedeck-backup.json">Download backup</a>

    <div class="row">
      <input type="file" accept=".json" onchange={onRestoreFileChange} disabled={restoring} />
      <button onclick={restore} disabled={!restoreFile || restoring}>
        {restoring ? "Restoring..." : "Restore"}
      </button>
    </div>
    {#if restoreError}
      <p class="error">{restoreError}</p>
    {:else if restoreResult}
      <p class="hint">{restoreResult}</p>
    {/if}
  {/if}
</div>

<style>
  .settings {
    text-align: left;
    margin-top: 1.5rem;
    padding-top: 1.5rem;
    border-top: 1px solid #e5e7eb;
  }

  h2 {
    margin: 0 0 0.75rem;
    font-size: 1.125rem;
  }

  h3 {
    margin: 1.25rem 0 0.5rem;
    font-size: 1rem;
  }

  p {
    margin: 0 0 0.5rem;
  }

  label {
    display: block;
    font-size: 0.875rem;
    margin-bottom: 0.25rem;
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

  input[type="file"] {
    flex: 1;
  }

  .hint {
    color: #4b5563;
    font-size: 0.875rem;
  }

  .error {
    color: #b91c1c;
    font-size: 0.875rem;
  }

  .button-link {
    display: inline-block;
    margin-bottom: 0.75rem;
    padding: 0.4rem 0.75rem;
    background: #f3f4f6;
    border: 1px solid #d1d5db;
    border-radius: 0.375rem;
    color: inherit;
    text-decoration: none;
    font-size: 0.875rem;
  }
</style>
