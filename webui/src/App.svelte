<script lang="ts">
  import Diagnostics from "./lib/Diagnostics.svelte";
  import PasswordForm from "./lib/PasswordForm.svelte";

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

  async function loadStatus() {
    try {
      const response = await fetch("/api/auth/status");
      if (!response.ok) {
        error = `Request failed: ${response.status}`;
        return;
      }
      error = undefined;
      status = (await response.json()) as AuthStatus;
    } catch (err) {
      error = String(err);
    }
  }

  async function logout() {
    await fetch("/api/auth/logout", { method: "POST" });
    await loadStatus();
  }

  loadStatus();
</script>

<div class="card">
  <h1>HomeDeck</h1>

  {#if error}
    <p class="error">Error: {error}</p>
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
    <Diagnostics />
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
