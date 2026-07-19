<script lang="ts">
  // Proves the real pipeline end to end (Svelte -> Vite -> embedded/served
  // static assets -> a real browser fetch against AdminAuthService's
  // actual endpoint) - not real UI yet, see
  // docs/architecture/web-ui.md#status.
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
      status = (await response.json()) as AuthStatus;
    } catch (err) {
      error = String(err);
    }
  }

  loadStatus();
</script>

<h1>HomeDeck Web UI</h1>
<p>The real admin/settings/diagnostics screens replace this page - see docs/architecture/web-ui.md.</p>

{#if error}
  <p>Error: {error}</p>
{:else if status}
  <p>Password set: {status.passwordSet}</p>
  <p>Authenticated: {status.authenticated}</p>
{:else}
  <p>Loading...</p>
{/if}
