<script lang="ts">
  import { findSetting, loadJson, postJson, type SettingEntry } from "./api";
  import { kodiHostError } from "./kodiValidation";
  import { tripGuard } from "./guardedAction";

  // Kodi module configuration (see docs/architecture/kodi.md and
  // docs/decisions/ADR-0030-kodi-jsonrpc-transport.md). Two mutually
  // exclusive keys under the generic /api/settings API: `instance_uuid`
  // (a discovered instance, keyed by its stable mDNS uuid) or `host` (a
  // manual address). Saving one clears the other. Plus core/kodi_routes.h's
  // status/reconnect endpoints. No credential - the 9090 API is
  // unauthenticated.
  const kModuleId = "kodi";
  const kHostKey = "host";
  const kInstanceUuidKey = "instance_uuid";
  const kSchemaVersion = 1;
  const kManualChoice = "__manual__";

  interface KodiDiscovered {
    name: string;
    host: string;
    uuid: string;
  }

  interface KodiStatus {
    state: "disconnected" | "connecting" | "connected" | "error";
    hasStatus: boolean;
    resolvedHost: string;
    discovered: KodiDiscovered[];
    appVersion: string;
    volume: number;
    muted: boolean;
    nowPlaying: {
      playback: "inactive" | "playing" | "paused";
      title: string;
      showTitle: string;
      season: number;
      episode: number;
    };
  }

  let error: string | undefined = $state(undefined);
  let loaded = $state(false);
  let loading = $state(false);

  // Radio-group value: a discovered instance's uuid, or kManualChoice.
  let choice = $state("");
  let manualHost = $state("");

  let saving = $state(false);
  let saveError: string | undefined = $state(undefined);
  let saved = $state(false);

  let status: KodiStatus | undefined = $state(undefined);
  let statusError: string | undefined = $state(undefined);
  let statusLoading = $state(false);

  async function loadSettings() {
    if (tripGuard(() => loading, () => (loading = true))) return;
    const result = await loadJson<SettingEntry[]>("/api/settings");
    loading = false;
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    const host = findSetting(result.data, kModuleId, kHostKey) ?? "";
    const uuid = findSetting(result.data, kModuleId, kInstanceUuidKey) ?? "";
    if (host !== "") {
      choice = kManualChoice;
      manualHost = host;
    } else {
      choice = uuid; // "" when nothing is selected yet
    }
    loaded = true;
  }

  async function loadStatus() {
    if (tripGuard(() => statusLoading, () => (statusLoading = true))) return;
    const result = await loadJson<KodiStatus>("/api/kodi/status");
    statusLoading = false;
    if (result.error !== undefined) {
      statusError = result.error;
      return;
    }
    statusError = undefined;
    status = result.data;
  }

  async function save() {
    let hostValue = "";
    let uuidValue = "";
    if (choice === kManualChoice) {
      hostValue = manualHost.trim();
      const validationError = kodiHostError(hostValue);
      if (validationError) {
        saveError = validationError;
        saved = false;
        return;
      }
    } else {
      uuidValue = choice;
    }

    if (tripGuard(() => saving, () => (saving = true))) return;
    saveError = undefined;
    saved = false;

    // Two keys, written in turn - one identifies the target, the other
    // must be cleared or the connection loop would still see it. Not
    // atomic: if the first write lands and the second fails (a network
    // drop between the two calls), the two mutually-exclusive settings
    // are left inconsistent until the user retries - the index check
    // below tells them that happened rather than showing the same
    // generic error either write would produce.
    const writes: [string, string][] = [
      [kHostKey, hostValue],
      [kInstanceUuidKey, uuidValue],
    ];
    for (let i = 0; i < writes.length; i++) {
      const [key, value] = writes[i];
      const result = await postJson("/api/settings", {
        module: kModuleId,
        key,
        value,
        schemaVersion: kSchemaVersion,
      });
      if (!result.ok) {
        saving = false;
        const reason =
          result.kind === "network"
            ? result.message
            : result.body.error === "invalid_value"
              ? "Not a valid Kodi address."
              : result.body.error === "value_too_long"
                ? "Kodi address is too long."
                : `Save failed: ${result.status}`;
        saveError =
          i === 0 ? reason : `Saved the new selection, but failed to clear the previous one - save again. (${reason})`;
        return;
      }
    }
    saving = false;
    saved = true;
    if (choice === kManualChoice) {
      manualHost = hostValue;
    }
    status = undefined;
    await postJson("/api/kodi/reconnect");
    loadStatus();
  }

  function stateLabel(state: KodiStatus["state"]): string {
    switch (state) {
      case "disconnected":
        return "Not connected";
      case "connecting":
        return "Connecting...";
      case "connected":
        return "Connected";
      case "error":
        return "Not reachable - retrying";
    }
  }

  function nowPlayingSummary(np: KodiStatus["nowPlaying"]): string | undefined {
    if (np.playback === "inactive") return undefined;
    const name = np.showTitle || np.title || "Something";
    const verb = np.playback === "paused" ? "Paused" : "Playing";
    return `${verb}: ${name}`;
  }

  loadSettings();
  loadStatus();
</script>

<div class="section">
  <h3>Kodi</h3>
  {#if error}
    <p class="error" aria-live="polite">Error: {error}</p>
    <button onclick={loadSettings} disabled={loading}>Retry</button>
  {:else if !loaded}
    <p class="hint">Loading...</p>
  {:else}
    <fieldset>
      <legend>Which Kodi?</legend>
      {#if status && status.discovered.length > 0}
        {#each status.discovered as instance (instance.uuid)}
          <label class="choice">
            <input type="radio" name="kodi-instance" bind:group={choice} value={instance.uuid} disabled={saving} />
            {instance.name} <span class="hint">({instance.host})</span>
          </label>
        {/each}
      {:else}
        <p class="hint">
          No Kodi found on the network yet. Open Kodi on the device (its remote-control API is only reachable while
          it's running), or enter an address below. Discovery refreshes every few seconds.
        </p>
      {/if}
      <label class="choice">
        <input type="radio" name="kodi-instance" bind:group={choice} value={kManualChoice} disabled={saving} />
        Enter an address manually
      </label>
      {#if choice === kManualChoice}
        <input
          type="text"
          aria-label="Kodi address"
          placeholder="Kodi IP address or hostname"
          maxlength="255"
          bind:value={manualHost}
          disabled={saving}
          oninput={() => {
            saved = false;
            saveError = undefined;
          }}
        />
      {/if}
    </fieldset>
    <div class="row">
      <button onclick={save} disabled={saving || choice === ""}>
        {saving ? "Saving..." : "Save"}
      </button>
    </div>
    <p class="hint">No password or account needed - Kodi's local control API has no authentication step.</p>
    {#if saveError}
      <p class="error" aria-live="polite">{saveError}</p>
    {:else if saved}
      <p class="hint" aria-live="polite">Saved.</p>
    {/if}
  {/if}

  <div class="row status-row">
    {#if statusError}
      <p class="error" aria-live="polite">Status error: {statusError}</p>
    {:else if status}
      <p>
        Status: <strong>{stateLabel(status.state)}</strong>
        {#if status.state === "connected"}
          {#if status.resolvedHost}&mdash; {status.resolvedHost}{/if}
          {#if nowPlayingSummary(status.nowPlaying) !== undefined}
            &mdash; <strong>{nowPlayingSummary(status.nowPlaying)}</strong>
          {/if}
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

  fieldset {
    border: 1px solid #d1d5db;
    border-radius: 0.375rem;
    padding: 0.5rem 0.75rem;
    margin: 0 0 0.5rem;
  }

  legend {
    font-size: 0.875rem;
    color: #4b5563;
    padding: 0 0.25rem;
  }

  .choice {
    display: flex;
    align-items: center;
    gap: 0.4rem;
    margin: 0.25rem 0;
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
    width: 100%;
    padding: 0.4rem 0.5rem;
    border: 1px solid #d1d5db;
    border-radius: 0.375rem;
    margin-top: 0.25rem;
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
