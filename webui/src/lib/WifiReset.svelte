<script lang="ts">
  import { postJson } from "./api";

  // onWifiReset fires once /api/wifi/reset succeeds - see its own call
  // site in resetWifi() below for why this hands off to App.svelte
  // instead of rendering a "done" state locally. apSsid is undefined
  // when the response body didn't fully arrive before the device's own
  // reboot severed the connection carrying it (see resetWifi()'s own
  // comment) - the reset was still scheduled either way, so this is
  // still success, just without the SSID to show.
  interface Props {
    onWifiReset: (apSsid: string | undefined) => void;
  }
  let { onWifiReset }: Props = $props();

  // Reset Wi-Fi credentials (see core/wifi_routes.h) - added as a
  // diagnostic aid for reproducing the intermittent Wi-Fi-connect crash
  // documented in docs/architecture/hardware.md#wi-fi-bring-up, without a
  // full tools/factory-reset.sh erase-and-reflash cycle per attempt. An
  // explicit confirm step first, matching this page's own crash/core-
  // dump section's "nothing happens by accident" tone rather than a
  // native window.confirm() dialog, which nothing else in this codebase
  // uses - but no separate confirmed reboot step after that: the device
  // reboots automatically once the reset is scheduled (see
  // wifi_routes.h's own WifiResetFn comment for why a reboot isn't
  // optional here, and why a second confirmed click - the way OTA offers
  // one - could never actually be pressed in time regardless).
  //
  // On success this doesn't render its own "done" state - the device is
  // rebooting, this browser's session and the LAN address it's talking
  // to are both about to become invalid, and every other panel on this
  // page (Settings, OTA, the rest of Diagnostics) would otherwise sit
  // there looking normal while actually unreachable. onWifiReset() hands
  // off to App.svelte instead, which replaces the entire authenticated
  // view with one dedicated message - not a page redirect (the device
  // won't be reachable at this address to serve one).
  type WifiResetState = "idle" | "confirming" | "resetting" | "error";
  let wifiResetState: WifiResetState = $state("idle");
  let wifiResetError: string | undefined = $state(undefined);

  async function resetWifi() {
    // Guards against a double-fired click event reaching this function
    // twice before Svelte's own reactive re-render removes the confirm
    // button that's about to become invalid the moment `wifiResetState`
    // changes below - a `disabled` binding on the button itself can't
    // express this, since the button only renders in the "confirming"
    // state and unmounts on the same state change that would need to
    // disable it.
    if (wifiResetState === "resetting") return;
    wifiResetState = "resetting";
    wifiResetError = undefined;
    const result = await postJson<{ apSsid: string }>("/api/wifi/reset");
    if (!result.ok) {
      // A network-level failure here (as opposed to a real HTTP error
      // response below) can still mean the device already rebooted
      // before this fetch's own connection fully settled - the same
      // "usually the expected outcome" reasoning Ota.svelte's reboot()
      // applies to its own post-reboot fetch - but unlike that case,
      // failure here is also a plausible real error (the crash this
      // action exists to reproduce, mid-request), so it's still surfaced
      // rather than assumed benign.
      wifiResetState = "error";
      wifiResetError =
        result.kind === "network"
          ? result.message
          : result.body.error === "unauthenticated"
            ? "Session expired - please log in again."
            : result.body.error === "reset_failed"
              ? "Wi-Fi reset failed - try again."
              : `Reset failed: ${result.status}`;
      return;
    }
    onWifiReset(result.data?.apSsid);
  }
</script>

<div class="wifi-reset">
  <h2>Wi-Fi</h2>
  {#if wifiResetState === "confirming"}
    <p class="hint">
      This clears the stored Wi-Fi network and password and reboots the device into SoftAP setup mode. Settings,
      secrets, and the admin password are not affected.
    </p>
    <button class="danger" onclick={resetWifi}>Yes, reset Wi-Fi credentials and reboot</button>
    <button class="secondary" onclick={() => (wifiResetState = "idle")}>Cancel</button>
  {:else}
    <p class="hint">
      Clears the device's stored Wi-Fi network and password, then reboots it into SoftAP setup mode.
    </p>
    <button onclick={() => (wifiResetState = "confirming")} disabled={wifiResetState === "resetting"}>
      {wifiResetState === "resetting" ? "Resetting..." : "Reset Wi-Fi credentials"}
    </button>
    {#if wifiResetState === "error"}
      <p class="error" aria-live="polite">{wifiResetError}</p>
    {/if}
  {/if}
</div>

<style>
  .wifi-reset {
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

  button.danger {
    background: #b91c1c;
    color: white;
  }
</style>
