<script lang="ts">
  import { downloadFile, postJson } from "./api";

  // Settings backup/restore (see ADR-0023-settings-backup-api.md). The
  // backend API is generic (any module/key) - the download is a
  // complete "see everything" escape hatch for debugging, independent
  // of which settings sections this page shows.
  let restoreFile: File | undefined = $state(undefined);
  let restoring = $state(false);
  let restoreResult: string | undefined = $state(undefined);
  let restoreError: string | undefined = $state(undefined);
  let downloading = $state(false);
  let downloadError: string | undefined = $state(undefined);

  function onRestoreFileChange(event: Event) {
    const input = event.target as HTMLInputElement;
    restoreFile = input.files?.[0];
    restoreResult = undefined;
    restoreError = undefined;
  }

  async function downloadBackup() {
    downloading = true;
    downloadError = undefined;
    const result = await downloadFile("/api/backup", "homedeck-backup.json");
    downloading = false;
    downloadError = result.error;
  }

  async function restore() {
    if (!restoreFile) return;
    restoring = true;
    restoreResult = undefined;
    restoreError = undefined;
    try {
      const fileBody = await restoreFile.text();
      const result = await postJson<{ applied: number; failed: unknown[]; rejected: unknown[] }>(
        "/api/backup/restore",
        fileBody,
      );
      if (!result.ok) {
        restoreError =
          result.kind === "network"
            ? result.message
            : result.body.error === "invalid_request"
              ? "The selected file isn't valid backup JSON."
              : result.body.error === "missing_field"
                ? "The selected file is missing its settings list."
                : `Restore failed: ${result.status}`;
        return;
      }
      const data = result.data;
      if (data === undefined) {
        restoreError = "The server accepted the restore but sent back an unreadable response.";
        return;
      }
      restoreResult = `Restored ${data.applied} setting${data.applied === 1 ? "" : "s"}${
        data.failed.length > 0 ? `, ${data.failed.length} failed` : ""
      }${data.rejected.length > 0 ? `, ${data.rejected.length} rejected as protected` : ""}.`;
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
  <button class="button-link" onclick={downloadBackup} disabled={downloading}>
    {downloading ? "Downloading..." : "Download backup"}
  </button>
  {#if downloadError}
    <p class="error" aria-live="polite">{downloadError}</p>
  {/if}

  <div class="row">
    <input
      type="file"
      accept=".json"
      aria-label="Backup file to restore"
      onchange={onRestoreFileChange}
      disabled={restoring}
    />
    <button onclick={restore} disabled={!restoreFile || restoring}>
      {restoring ? "Restoring..." : "Restore"}
    </button>
  </div>
  {#if restoreError}
    <p class="error" aria-live="polite">{restoreError}</p>
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
    font-family: inherit;
    cursor: pointer;
  }

  .button-link:disabled {
    cursor: default;
    opacity: 0.6;
  }
</style>
