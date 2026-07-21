<script lang="ts">
  // Crash/reboot diagnostics (see docs/architecture/diagnostics.md and
  // ADR-0013), live battery/power state (see
  // docs/architecture/hardware.md#power), and structured logs (see
  // ADR-0019) - real diagnostic data today. Module status, connection
  // state, and error reporting are still unbuilt (no modules exist
  // yet), so there's nothing else to show here yet.
  interface DiagnosticsStatus {
    resetReason: string;
    hasCoreDump: boolean;
    batteryPercent: number;
    externalPowerConnected: boolean;
    batteryPresent: boolean;
  }

  interface LogEntry {
    timestamp: number;
    level: string;
    component: string;
    message: string;
  }

  // $state(undefined)'s generic infers from the argument, not the LHS
  // annotation, quietly typing these as `undefined` instead of the wider
  // union above - explicit type arguments here are load-bearing, not
  // decorative (see the log-entries $derived expressions below, which
  // svelte-check rejected outright without this).
  let status: DiagnosticsStatus | undefined = $state<DiagnosticsStatus | undefined>(undefined);
  let error: string | undefined = $state(undefined);

  let logs: LogEntry[] | undefined = $state<LogEntry[] | undefined>(undefined);
  let logsError: string | undefined = $state(undefined);
  // Filtering happens client-side against the already-fetched array,
  // not server-side query params - the whole log is small enough (see
  // ADR-0019's rotation cap) that this is simpler than a backend filter
  // API, matching this file's own existing fetch-then-render pattern.
  let levelFilter = $state("all");
  let componentFilter = $state("all");

  async function load() {
    try {
      const response = await fetch("/api/diagnostics");
      if (!response.ok) {
        error = `Request failed: ${response.status}`;
        return;
      }
      error = undefined;
      status = (await response.json()) as DiagnosticsStatus;
    } catch (err) {
      error = String(err);
    }
  }

  async function loadLogs() {
    try {
      const response = await fetch("/api/diagnostics/logs");
      if (!response.ok) {
        logsError = `Request failed: ${response.status}`;
        return;
      }
      logsError = undefined;
      logs = (await response.json()) as LogEntry[];
    } catch (err) {
      logsError = String(err);
    }
  }

  function formatTimestamp(unixSeconds: number): string {
    return new Date(unixSeconds * 1000).toLocaleString();
  }

  let components = $derived(logs ? Array.from(new Set(logs.map((entry) => entry.component))).sort() : []);
  let filteredLogs = $derived(
    (logs ?? [])
      .filter((entry) => levelFilter === "all" || entry.level === levelFilter)
      .filter((entry) => componentFilter === "all" || entry.component === componentFilter)
      // Newest first - matches how a developer actually wants to scan
      // recent activity, not the storage order ReadAll() returns.
      .slice()
      .reverse(),
  );

  load();
  loadLogs();
</script>

<div class="diagnostics">
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
        <a href="/api/diagnostics/coredump" download="coredump.bin">Download</a>
      </p>
    {:else}
      <p class="hint">No core dump present.</p>
    {/if}
  {/if}

  <h2>Logs</h2>
  {#if logsError}
    <p class="error">Error: {logsError}</p>
  {:else if !logs}
    <p class="hint">Loading...</p>
  {:else if logs.length === 0}
    <p class="hint">No log entries yet.</p>
  {:else}
    <div class="log-filters">
      <select bind:value={levelFilter}>
        <option value="all">All levels</option>
        <option value="debug">Debug</option>
        <option value="info">Info</option>
        <option value="warning">Warning</option>
        <option value="error">Error</option>
      </select>
      <select bind:value={componentFilter}>
        <option value="all">All components</option>
        {#each components as component (component)}
          <option value={component}>{component}</option>
        {/each}
      </select>
    </div>
    {#if filteredLogs.length === 0}
      <p class="hint">No entries match the current filter.</p>
    {:else}
      <ul class="log-entries">
        {#each filteredLogs as entry (entry.timestamp + entry.component + entry.message)}
          <li class="log-entry log-level-{entry.level}">
            <div class="log-meta">
              <span class="log-timestamp">{formatTimestamp(entry.timestamp)}</span>
              <span class="log-level">{entry.level}</span>
              <span class="log-component">{entry.component}</span>
            </div>
            <div class="log-message">{entry.message}</div>
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</div>

<style>
  .diagnostics {
    text-align: left;
    margin-top: 1.5rem;
    padding-top: 1.5rem;
    border-top: 1px solid #e5e7eb;
  }

  h2 {
    margin: 1.5rem 0 0.75rem;
    font-size: 1.125rem;
  }

  h2:first-child {
    margin-top: 0;
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

  a {
    color: #2563eb;
  }

  .log-filters {
    display: flex;
    gap: 0.5rem;
    margin-bottom: 0.75rem;
  }

  .log-entries {
    list-style: none;
    margin: 0;
    padding: 0;
    max-height: 20rem;
    overflow-y: auto;
  }

  .log-entry {
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
    padding: 0.35rem 0;
    border-bottom: 1px solid #f3f4f6;
    font-size: 0.85rem;
  }

  /* Timestamp/level/component share a row - they're all short, fixed-
     width labels. The message (see .log-message below) gets its own
     full-width line instead of squeezing into what's left of this row,
     since it's normally the longest, most important field to read. */
  .log-meta {
    display: flex;
    gap: 0.5rem;
  }

  .log-timestamp {
    color: #6b7280;
    white-space: nowrap;
  }

  .log-level {
    text-transform: uppercase;
    font-weight: 600;
    white-space: nowrap;
  }

  .log-level-error .log-level {
    color: #b91c1c;
  }

  .log-level-warning .log-level {
    color: #b45309;
  }

  .log-level-info .log-level {
    color: #2563eb;
  }

  .log-level-debug .log-level {
    color: #6b7280;
  }

  .log-component {
    color: #4b5563;
    white-space: nowrap;
  }

  .log-message {
    word-break: break-word;
  }
</style>
