<script lang="ts">
  // Settings/backup (see docs/architecture/web-ui.md#scope and
  // docs/decisions/ADR-0023-settings-backup-api.md). The backend API is
  // generic (any module/key), but this shows only what's real today -
  // device name and weather location, not a generic raw key-value
  // editor; the backup download already gives a complete "see
  // everything" escape hatch for debugging.
  interface SettingEntry {
    module: string;
    key: string;
    value: string;
    schemaVersion: number;
  }

  interface GeocodeResult {
    name: string;
    admin1: string;
    country: string;
    latitude: number;
    longitude: number;
  }

  const kModuleId = "core";
  const kDeviceNameKey = "device_name";
  const kDeviceNameSchemaVersion = 1;

  const kWeatherModuleId = "weather";
  const kWeatherSchemaVersion = 1;

  let error: string | undefined = $state(undefined);
  let deviceName = $state("");
  let deviceNameLoaded = $state(false);
  let savingDeviceName = $state(false);
  let deviceNameError: string | undefined = $state(undefined);
  let deviceNameSaved = $state(false);

  // The second real user-facing setting, after device name - see
  // docs/architecture/dashboard.md's Weather source section and
  // ADR-0008. Backed by the same generic /api/settings endpoint (three
  // plain keys: latitude/longitude/display_name), not a dedicated
  // weather-config endpoint - no per-module schema exists or is needed.
  let weatherDisplayName = $state("");
  let weatherQuery = $state("");
  let weatherSearching = $state(false);
  let weatherSearchError: string | undefined = $state(undefined);
  let weatherResults: GeocodeResult[] = $state([]);
  let weatherSaving = $state(false);
  let weatherSaveError: string | undefined = $state(undefined);

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
      const weatherName = entries.find(
        (entry) => entry.module === kWeatherModuleId && entry.key === "display_name",
      );
      weatherDisplayName = weatherName?.value ?? "";
      deviceNameLoaded = true;
    } catch (err) {
      error = String(err);
    }
  }

  async function searchWeatherLocation() {
    if (!weatherQuery.trim()) return;
    weatherSearching = true;
    weatherSearchError = undefined;
    weatherResults = [];
    try {
      const response = await fetch(`/api/weather/geocode?query=${encodeURIComponent(weatherQuery)}`);
      if (!response.ok) {
        weatherSearchError = `Search failed: ${response.status}`;
        return;
      }
      const body = (await response.json()) as { results: GeocodeResult[] };
      weatherResults = body.results;
      if (weatherResults.length === 0) {
        weatherSearchError = "No matches found.";
      }
    } catch (err) {
      weatherSearchError = String(err);
    } finally {
      weatherSearching = false;
    }
  }

  async function selectWeatherLocation(result: GeocodeResult) {
    weatherSaving = true;
    weatherSaveError = undefined;
    const label = [result.name, result.admin1, result.country].filter(Boolean).join(", ");
    try {
      for (const [key, value] of [
        ["latitude", String(result.latitude)],
        ["longitude", String(result.longitude)],
        ["display_name", label],
      ]) {
        const response = await fetch("/api/settings", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ module: kWeatherModuleId, key, value, schemaVersion: kWeatherSchemaVersion }),
        });
        if (!response.ok) {
          weatherSaveError = `Save failed: ${response.status}`;
          return;
        }
      }
      weatherDisplayName = label;
      weatherResults = [];
      weatherQuery = "";
      // Without this, the dashboard widget would silently wait out the
      // rest of the real ~30-minute poll interval before showing
      // anything for the location just chosen - fire-and-forget, the
      // widget picks up the result via its own WeatherUpdatedEvent
      // subscription once the triggered fetch completes.
      fetch("/api/weather/refresh", { method: "POST" }).catch(() => {});
    } catch (err) {
      weatherSaveError = String(err);
    } finally {
      weatherSaving = false;
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

    <h3>Weather</h3>
    {#if weatherDisplayName}
      <p class="hint">Current location: {weatherDisplayName}</p>
    {:else}
      <p class="hint">No location set - the dashboard's weather widget won't show a reading until one is chosen.</p>
    {/if}
    <div class="row">
      <input
        type="text"
        placeholder="Search for a city..."
        bind:value={weatherQuery}
        disabled={weatherSearching || weatherSaving}
        onkeydown={(event) => {
          if (event.key === "Enter") searchWeatherLocation();
        }}
      />
      <button
        onclick={searchWeatherLocation}
        disabled={weatherSearching || weatherSaving || weatherQuery.trim().length === 0}
      >
        {weatherSearching ? "Searching..." : "Search"}
      </button>
    </div>
    {#if weatherSearchError}
      <p class="error">{weatherSearchError}</p>
    {/if}
    {#if weatherSaveError}
      <p class="error">{weatherSaveError}</p>
    {/if}
    {#if weatherResults.length > 0}
      <ul class="weather-results">
        {#each weatherResults as result}
          <li>
            <span>{[result.name, result.admin1, result.country].filter(Boolean).join(", ")}</span>
            <button onclick={() => selectWeatherLocation(result)} disabled={weatherSaving}>Select</button>
          </li>
        {/each}
      </ul>
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

  .weather-results {
    list-style: none;
    margin: 0 0 0.75rem;
    padding: 0;
    border: 1px solid #e5e7eb;
    border-radius: 0.375rem;
    overflow: hidden;
  }

  .weather-results li {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
    padding: 0.4rem 0.6rem;
    font-size: 0.875rem;
    border-bottom: 1px solid #e5e7eb;
  }

  .weather-results li:last-child {
    border-bottom: none;
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
