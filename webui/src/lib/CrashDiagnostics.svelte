<script lang="ts">
  import { downloadFile, loadJson, type BatteryStatus } from "./api";

  // Crash/reboot diagnostics (see docs/architecture/diagnostics.md and
  // ADR-0013) and live battery/power state (see
  // docs/architecture/hardware.md#power) - real diagnostic data today.
  // Module status, connection state, and error reporting are still
  // unbuilt (no modules exist yet), so there's nothing else to show here
  // yet.
  interface DiagnosticsStatus extends BatteryStatus {
    resetReason: string;
    hasCoreDump: boolean;
  }

  let status: DiagnosticsStatus | undefined = $state<DiagnosticsStatus | undefined>(undefined);
  let error: string | undefined = $state(undefined);
  let downloadingCoreDump = $state(false);
  let coreDumpError: string | undefined = $state(undefined);

  async function load() {
    const result = await loadJson<DiagnosticsStatus>("/api/diagnostics");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    status = result.data;
  }

  async function downloadCoreDump() {
    downloadingCoreDump = true;
    coreDumpError = undefined;
    const result = await downloadFile("/api/diagnostics/coredump", "coredump.bin");
    downloadingCoreDump = false;
    coreDumpError = result.error;
  }

  load();
</script>

<div class="crash-diagnostics">
  <h2>Diagnostics</h2>
  {#if error}
    <p class="error">Error: {error}</p>
  {:else if !status}
    <p class="hint">Loading...</p>
  {:else}
    {#if status.batteryPresent}
      <p>
        Battery: <strong>{status.batteryPercent}%</strong>
        ({status.externalPowerConnected ? "external power connected" : "on battery"})
      </p>
    {:else}
      <p>
        No battery installed
        ({status.externalPowerConnected ? "running on external power" : "no power source detected"})
      </p>
    {/if}
    <p>Last reset reason: <strong>{status.resetReason}</strong></p>
    {#if status.hasCoreDump}
      <p>
        A core dump is available from the last crash.
        <button class="link-button" onclick={downloadCoreDump} disabled={downloadingCoreDump}>
          {downloadingCoreDump ? "Downloading..." : "Download"}
        </button>
      </p>
      {#if coreDumpError}
        <p class="error">{coreDumpError}</p>
      {/if}
    {:else}
      <p class="hint">No core dump present.</p>
    {/if}
  {/if}
</div>

<style>
  .crash-diagnostics {
    text-align: left;
    margin-top: 1.5rem;
    padding-top: 1.5rem;
    border-top: 1px solid #e5e7eb;
  }

  h2 {
    margin: 0 0 0.75rem;
    font-size: 1.125rem;
  }

  p {
    margin: 0 0 0.5rem;
  }

  .hint {
    color: #4b5563;
  }

  .error {
    color: #b91c1c;
  }

  .link-button {
    background: none;
    border: none;
    padding: 0;
    color: #2563eb;
    font-family: inherit;
    font-size: inherit;
    cursor: pointer;
  }

  .link-button:disabled {
    cursor: default;
    opacity: 0.6;
  }
</style>
