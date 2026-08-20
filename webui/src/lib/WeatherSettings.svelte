<script lang="ts">
  import { findSetting, getJson, loadJson, postJson, type SettingEntry } from "./api";

  // The dashboard's weather source (see docs/architecture/dashboard.md's
  // Weather source section and ADR-0008). Backed by the generic
  // /api/settings endpoint (three plain keys: latitude/longitude/
  // display_name), not a dedicated weather-config endpoint - no
  // per-module schema exists or is needed.
  interface GeocodeResult {
    name: string;
    admin1: string;
    country: string;
    latitude: number;
    longitude: number;
  }

  const kWeatherModuleId = "weather";
  const kWeatherSchemaVersion = 1;

  let error: string | undefined = $state(undefined);
  let loaded = $state(false);
  let displayName = $state("");
  let query = $state("");
  let searching = $state(false);
  let searchError: string | undefined = $state(undefined);
  let results: GeocodeResult[] = $state([]);
  let saving = $state(false);
  let saveError: string | undefined = $state(undefined);

  async function loadWeatherLocation() {
    const result = await loadJson<SettingEntry[]>("/api/settings");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    displayName = findSetting(result.data, kWeatherModuleId, "display_name") ?? "";
    loaded = true;
  }

  async function searchLocation() {
    if (!query.trim()) return;
    searching = true;
    searchError = undefined;
    results = [];
    const result = await getJson<{ results: GeocodeResult[] }>(
      `/api/weather/geocode?query=${encodeURIComponent(query)}`,
    );
    searching = false;
    if (!result.ok) {
      searchError =
        result.kind === "network"
          ? result.message
          : result.body.error === "upstream_failed"
            ? "Couldn't reach the location search service."
            : result.body.error === "upstream_invalid_response"
              ? "The location search service returned an unexpected response."
              : result.body.error === "query_too_long"
                ? "Search text is too long."
                : `Search failed: ${result.status}`;
      return;
    }
    results = result.data.results;
    if (results.length === 0) {
      searchError = "No matches found.";
    }
  }

  async function selectLocation(result: GeocodeResult) {
    // Same double-fired-click guard as BackupSettings.svelte's restore()/
    // WifiReset.svelte's resetWifi() - this button stays mounted with a
    // `disabled` binding rather than unmounting, but a second click
    // event dispatched before Svelte reactively applies that attribute
    // could still reach here before this function's own synchronous
    // `saving = true` below takes effect in the DOM.
    if (saving) return;
    saving = true;
    saveError = undefined;
    const label = [result.name, result.admin1, result.country].filter(Boolean).join(", ");
    // No batch/transactional settings endpoint exists (POST
    // /api/backup/restore is the only other multi-key writer, and it's
    // explicitly not atomic either - see settings_routes.cpp's own
    // comment). Rather than stop at the first failure, which would leave
    // whichever keys come later always untried, every key is attempted
    // and every failure reported together - matching restore's own
    // applied/failed transparency instead of silently leaving some keys
    // on their old value with no indication which.
    const failedKeys: string[] = [];
    for (const [key, value] of [
      ["latitude", String(result.latitude)],
      ["longitude", String(result.longitude)],
      ["display_name", label],
    ] as const) {
      const postResult = await postJson("/api/settings", {
        module: kWeatherModuleId,
        key,
        value,
        schemaVersion: kWeatherSchemaVersion,
      });
      if (!postResult.ok) {
        failedKeys.push(key);
      }
    }
    saving = false;
    if (failedKeys.length > 0) {
      saveError = `Location only partially saved - failed to save: ${failedKeys.join(", ")}. Select it again to retry.`;
      return;
    }
    displayName = label;
    results = [];
    query = "";
    // Without this, the dashboard widget would silently wait out the
    // rest of the real ~30-minute poll interval before showing
    // anything for the location just chosen - fire-and-forget, the
    // widget picks up the result via its own WeatherUpdatedEvent
    // subscription once the triggered fetch completes. postJson() (not a
    // bare fetch()) so a session that lapsed at this exact moment still
    // routes through notifyIfSessionExpired() like every other request in
    // this app - postJson() never throws, so no .catch() is needed.
    void postJson("/api/weather/refresh");
  }

  loadWeatherLocation();
</script>

<div class="section">
  <h3>Weather</h3>
  {#if error}
    <p class="error" aria-live="polite">Error: {error}</p>
    <button onclick={loadWeatherLocation}>Retry</button>
  {:else if !loaded}
    <p class="hint">Loading...</p>
  {:else}
    {#if displayName}
      <p class="hint">Current location: {displayName}</p>
    {:else}
      <p class="hint">No location set - the dashboard's weather widget won't show a reading until one is chosen.</p>
    {/if}
    <div class="row">
      <!-- maxlength matches the server's own kMaxGeocodeQueryLength
           (weather_routes.cpp) - without it, a pasted long query only
           fails after the round trip to the "query_too_long" error this
           screen already maps below, unlike HarmonySettings/
           DeviceNameSettings' own inputs, which fail fast locally. -->
      <input
        type="text"
        aria-label="Search for a weather location"
        placeholder="Search for a city..."
        maxlength="100"
        bind:value={query}
        disabled={searching || saving}
        onkeydown={(event) => {
          if (event.key === "Enter") searchLocation();
        }}
      />
      <button onclick={searchLocation} disabled={searching || saving || query.trim().length === 0}>
        {searching ? "Searching..." : "Search"}
      </button>
    </div>
    {#if searchError}
      <p class="error" aria-live="polite">{searchError}</p>
    {/if}
    {#if saveError}
      <p class="error" aria-live="polite">{saveError}</p>
    {/if}
    {#if results.length > 0}
      <ul class="weather-results">
        {#each results as result}
          <li>
            <span>{[result.name, result.admin1, result.country].filter(Boolean).join(", ")}</span>
            <button onclick={() => selectLocation(result)} disabled={saving}>Select</button>
          </li>
        {/each}
      </ul>
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
</style>
