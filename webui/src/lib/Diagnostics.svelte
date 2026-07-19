<script lang="ts">
  // Crash/reboot diagnostics (see docs/architecture/diagnostics.md and
  // ADR-0013) - the only diagnostic data that's real today. Structured
  // logs, module status, connection state, and error reporting are all
  // still unbuilt (no modules, no general logging facility yet), so
  // there's nothing else to show here yet.
  interface DiagnosticsStatus {
    resetReason: string;
    hasCoreDump: boolean;
  }

  let status: DiagnosticsStatus | undefined = $state(undefined);
  let error: string | undefined = $state(undefined);

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

  load();
</script>

<div class="diagnostics">
  <h2>Diagnostics</h2>
  {#if error}
    <p class="error">Error: {error}</p>
  {:else if !status}
    <p class="hint">Loading...</p>
  {:else}
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
</div>

<style>
  .diagnostics {
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

  a {
    color: #2563eb;
  }
</style>
