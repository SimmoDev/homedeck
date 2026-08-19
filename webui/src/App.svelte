<script lang="ts">
  import { loadJson, postJson, setSessionExpiredHandler } from "./lib/api";
  import Diagnostics from "./lib/Diagnostics.svelte";
  import Ota from "./lib/Ota.svelte";
  import PasswordForm from "./lib/PasswordForm.svelte";
  import Settings from "./lib/Settings.svelte";

  // The real first-login-sets-password / session-login flow (see
  // docs/architecture/web-ui.md#admin-password and ADR-0007) - status
  // drives which of the three states below is shown; nothing here has
  // client-side auth state of its own, since AdminAuthService's session
  // cookie (HttpOnly) is the actual source of truth.
  interface AuthStatus {
    passwordSet: boolean;
    authenticated: boolean;
  }

  let status: AuthStatus | undefined = $state(undefined);
  let error: string | undefined = $state(undefined);

  // Set once Diagnostics.svelte's Wi-Fi-reset action succeeds - see its
  // own onWifiReset comment for why that hands off here instead of
  // rendering its own "done" state. Checked first, ahead of the normal
  // status-driven chain below: the device is rebooting, so `status`
  // (and everything built on it - Settings/OTA/Diagnostics, all still
  // mounted underneath) reflects a session and a LAN address that are
  // both about to stop working, not a state worth returning to by
  // itself clearing or timing out. There's deliberately no way back out
  // of this view - a page reload is the correct next step, once the
  // device is actually reachable again at whatever address it gets from
  // reprovisioning.
  //
  // wifiResetSsid is separate from wifiResetTriggered (rather than one
  // `string | undefined`) because "reset happened, SSID unknown" and
  // "no reset happened yet" are both otherwise indistinguishable -
  // WifiReset.svelte's own onWifiReset can pass undefined for the SSID
  // when the device's response body didn't fully arrive before its
  // reboot severed the connection carrying it, which is still a
  // successful reset, not a missing one.
  let wifiResetTriggered = $state(false);
  let wifiResetSsid: string | undefined = $state(undefined);

  async function loadStatus() {
    const result = await loadJson<AuthStatus>("/api/auth/status");
    if (result.error !== undefined) {
      error = result.error;
      return;
    }
    error = undefined;
    status = result.data;
  }

  async function logout() {
    // Result deliberately ignored, unlike every other postJson() call in
    // this app - a failed logout isn't user-actionable (there's no retry
    // affordance worth showing), and loadStatus() re-checking the real
    // session state afterward is correct either way: still logged in on
    // a network failure, logged out if the request actually landed.
    await postJson("/api/auth/logout");
    await loadStatus();
  }

  // Any authenticated request anywhere below this component (Settings,
  // Ota, Diagnostics, and their children) that gets a 401 back - a
  // lapsed session or one cleared by an OTA reboot - re-runs loadStatus()
  // here, dropping the whole app back to the login screen instead of
  // leaving that one component stuck on a dead-end error message.
  setSessionExpiredHandler(loadStatus);
  loadStatus();
</script>

<div class="card" class:card-wide={status?.authenticated && !wifiResetTriggered}>
  <h1>HomeDeck</h1>

  {#if wifiResetTriggered}
    <p class="hint">
      Wi-Fi credentials cleared. The device is rebooting into setup mode - connect to its
      {#if wifiResetSsid}
        <strong>{wifiResetSsid}</strong> Wi-Fi network
      {:else}
        Wi-Fi setup network
      {/if}
      from a computer or phone, or use the on-device touch screen, to reconfigure Wi-Fi. This page will not be
      reachable again until then.
    </p>
  {:else if error}
    <p class="error" aria-live="polite">Error: {error}</p>
    <button onclick={loadStatus}>Retry</button>
  {:else if !status}
    <p class="hint">Loading...</p>
  {:else if !status.passwordSet}
    <p class="hint">Set an admin password to continue.</p>
    <PasswordForm mode="setup" onStateChange={loadStatus} />
  {:else if !status.authenticated}
    <p class="hint">Log in to continue.</p>
    <PasswordForm mode="login" onStateChange={loadStatus} />
  {:else}
    <p class="hint">Logged in.</p>
    <button class="secondary" onclick={logout}>Log out</button>
    <Settings />
    <Ota />
    <Diagnostics
      onWifiReset={(apSsid) => {
        wifiResetTriggered = true;
        wifiResetSsid = apSsid;
      }}
    />
  {/if}
</div>

<style>
  .card {
    width: 100%;
    max-width: 22rem;
    padding: 2rem;
    background: white;
    border-radius: 0.75rem;
    box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1), 0 1px 2px rgba(0, 0, 0, 0.06);
    box-sizing: border-box;
  }

  /* Only the authenticated view (OTA + Diagnostics, including the Logs
     list) needs the extra width - the password setup/login form stays
     narrow, since a single input field doesn't benefit from it. */
  .card-wide {
    max-width: 40rem;
  }

  h1 {
    margin: 0 0 1rem;
    font-size: 1.5rem;
  }

  .hint {
    margin: 0 0 1rem;
    color: #4b5563;
  }

  .error {
    margin: 0 0 1rem;
    color: #b91c1c;
  }

  :global(button.secondary) {
    background: #e5e7eb;
    color: #1f2937;
  }
</style>
