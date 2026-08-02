<script lang="ts">
  import { loadJson, notifyIfSessionExpired, readErrorBody, type ApiErrorBody, type BatteryStatus } from "./api";

  // Firmware update (see docs/architecture/web-ui.md#ota and
  // docs/decisions/ADR-0005-power-and-sleep-model.md's OTA gate
  // decision). Deliberately uses XMLHttpRequest for the upload call,
  // not fetch - fetch() has no equivalent to
  // XMLHttpRequest.upload.onprogress, and a multi-MB firmware image is
  // worth real progress reporting.
  interface OtaStatus extends BatteryStatus {
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
  let rebootError: string | undefined = $state(undefined);

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
        return;
      }
      uploadState = "error";
      // xhr.status, not a fetch Response, so notifyIfSessionExpired()
      // is called directly here rather than through readErrorBody() -
      // same 401-drops-to-login behavior every other authenticated call
      // in this app gets.
      notifyIfSessionExpired(xhr.status);
      // xhr.responseText, not readErrorBody() (which takes a fetch
      // Response) - same {"error":...} shape core/ota_routes.cpp's
      // failure bodies share.
      let errorBody: ApiErrorBody = {};
      try {
        errorBody = JSON.parse(xhr.responseText) as ApiErrorBody;
      } catch {
        // Non-JSON body (e.g. a proxy error) - fall through to the
        // generic status message below.
      }
      uploadError =
        errorBody.error === "unauthenticated"
          ? "Session expired - please log in again."
          : errorBody.error === "gate_closed"
            ? `Update blocked: ${errorBody.reason ?? "gate closed"}.`
            : errorBody.error === "image_too_large"
              ? "The selected file is too large to be a valid firmware image."
              : errorBody.error === "write_failed"
                ? "The device failed to write the update - try again."
                : errorBody.error === "upload_in_progress"
                  ? "Another update is already uploading - wait for it to finish."
                  : `Upload failed: ${xhr.status}`;
    };
    xhr.onerror = () => {
      uploadState = "error";
      uploadError = "Upload failed: network error";
    };
    xhr.send(selectedFile);
  }

  async function reboot() {
    rebooting = true;
    rebootError = undefined;
    try {
      const response = await fetch("/api/ota/reboot", { method: "POST" });
      if (!response.ok) {
        rebooting = false;
        const body = await readErrorBody(response);
        rebootError =
          body.error === "unauthenticated"
            ? "Session expired - please log in again."
            : `Reboot request failed: ${response.status}`;
      }
    } catch {
      // A network-level failure (as opposed to a real HTTP error
      // response above) usually means the device already rebooted and
      // cut the connection - the expected outcome, not a real failure -
      // so `rebooting` stays true rather than resetting to an error.
    }
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
      {#if rebootError}
        <p class="error">{rebootError}</p>
      {/if}
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
