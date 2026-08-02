<script lang="ts">
  import { readErrorBody } from "./api";

  // Settings backup/restore (see ADR-0023-settings-backup-api.md). The
  // backend API is generic (any module/key) - the download link is a
  // complete "see everything" escape hatch for debugging, independent
  // of which settings sections this page shows.
  let restoreFile: File | undefined = $state(undefined);
  let restoring = $state(false);
  let restoreResult: string | undefined = $state(undefined);
  let restoreError: string | undefined = $state(undefined);

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
        const errorBody = await readErrorBody(response);
        restoreError =
          errorBody.error === "invalid_request"
            ? "The selected file isn't valid backup JSON."
            : errorBody.error === "missing_field"
              ? "The selected file is missing its settings list."
              : `Restore failed: ${response.status}`;
        return;
      }
      const result = (await response.json()) as { applied: number; failed: unknown[] };
      restoreResult = `Restored ${result.applied} setting${result.applied === 1 ? "" : "s"}${
        result.failed.length > 0 ? `, ${result.failed.length} failed` : ""
      }.`;
    } catch (err) {
      restoreError = String(err);
    } finally {
      restoring = false;
    }
  }
</script>

<div class="section">
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
</div>

<style>
  .section {
    text-align: left;
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
