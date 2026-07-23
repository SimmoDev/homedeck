<script lang="ts">
  import { loadJson } from "./api";

  // Firmware update (see docs/architecture/web-ui.md#ota and
  // docs/decisions/ADR-0005-power-and-sleep-model.md's OTA gate
  // decision). Deliberately uses XMLHttpRequest for the upload call,
  // not fetch - fetch() has no equivalent to
  // XMLHttpRequest.upload.onprogress, and a multi-MB firmware image is
  // worth real progress reporting.
  interface OtaStatus {
    batteryPercent: number;
    externalPowerConnected: boolean;
    batteryPresent: boolean;
    gateOpen: boolean;
    gateReason: string;
    runningVersion: string;
  }

  type UploadState = "idle" | "uploading" | "success" | "error";

  let status: OtaStatus | undefined = $state(undefined);
  let error: string | undefined = $state(undefined);
  let selectedFile: File | undefined = $state(undefined);
  let uploadState: UploadState = $state("idle");
  let uploadProgress = $state(0);
  let uploadError: string | undefined = $state(undefined);
  let rebooting = $state(false);

  async function loadStatus() {
    const result = await loadJson<OtaStatus>("/api/ota/status");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    status = result.data;
  }

  function onFileChange(event: Event) {
    const input = event.target as HTMLInputElement;
    selectedFile = input.files?.[0];
  }

  function upload() {
    if (!selectedFile) {
      return;
    }
    uploadState = "uploading";
    uploadProgress = 0;
    uploadError = undefined;

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota/upload");
    xhr.upload.onprogress = (event) => {
      if (event.lengthComputable) {
        uploadProgress = Math.round((event.loaded / event.total) * 100);
      }
    };
    xhr.onload = () => {
      if (xhr.status === 200) {
        uploadState = "success";
      } else {
        uploadState = "error";
        uploadError = `Upload failed: ${xhr.status}`;
      }
    };
    xhr.onerror = () => {
      uploadState = "error";
      uploadError = "Upload failed: network error";
    };
    xhr.send(selectedFile);
  }

  async function reboot() {
    rebooting = true;
    await fetch("/api/ota/reboot", { method: "POST" });
  }

  loadStatus();
</script>

<div class="ota">
  <h2>Firmware update</h2>
  {#if error}
    <p class="error">Error: {error}</p>
  {:else if !status}
    <p class="hint">Loading...</p>
  {:else}
    <p>Running version: <strong>{status.runningVersion}</strong></p>
    <p>
      {#if status.batteryPresent}
        Battery: {status.batteryPercent}%{status.externalPowerConnected ? ", external power connected" : ""}
      {:else}
        No battery installed, running on external power
      {/if}
    </p>
    {#if !status.gateOpen}
      <p class="hint">{status.gateReason}</p>
    {/if}

    {#if uploadState === "success"}
      <p>Upload complete. Reboot to apply the update.</p>
      <button onclick={reboot} disabled={rebooting}>
        {rebooting ? "Rebooting..." : "Reboot now"}
      </button>
    {:else}
      <input type="file" accept=".bin" onchange={onFileChange} disabled={uploadState === "uploading"} />
      <button
        onclick={upload}
        disabled={!status.gateOpen || !selectedFile || uploadState === "uploading"}
      >
        Upload
      </button>
      {#if uploadState === "uploading"}
        <p class="hint">Uploading... {uploadProgress}%</p>
      {:else if uploadState === "error"}
        <p class="error">{uploadError}</p>
      {/if}
    {/if}
  {/if}
</div>

<style>
  .ota {
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

  input[type="file"] {
    display: block;
    margin-bottom: 0.5rem;
  }
</style>
