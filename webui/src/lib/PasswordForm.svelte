<script lang="ts">
  import { postJson } from "./api";
  import { describeAuthError, validateSetupPassword } from "./passwordValidation";
  import { tripGuard } from "./guardedAction";

  // Shared by both first-login setup and ordinary login - same field,
  // same submit mechanics, differing only in endpoint/copy and setup's
  // extra confirm-password field (a safeguard, not decoration:
  // there's no password-recovery flow yet, so a setup typo is otherwise
  // unrecoverable without an NVS erase - see ADR-0007's accepted-risk
  // note).
  interface Props {
    mode: "setup" | "login";
    onStateChange: () => void;
  }
  let { mode, onStateChange }: Props = $props();

  let password = $state("");
  let confirmPassword = $state("");
  let error: string | undefined = $state(undefined);
  let submitting = $state(false);

  async function handleSubmit(event: SubmitEvent) {
    event.preventDefault();
    error = undefined;

    if (mode === "setup") {
      const validationError = validateSetupPassword(password, confirmPassword);
      if (validationError) {
        error = validationError;
        return;
      }
    }

    if (tripGuard(() => submitting, () => (submitting = true))) return;
    const result = await postJson(mode === "setup" ? "/api/auth/setup" : "/api/auth/login", { password });
    submitting = false;
    // already_set means another request won first (the real race
    // ADR-0007 accepts) - the password itself may now be wrong, but
    // the state genuinely changed, so re-checking status is still the
    // right move rather than treating this as this form's own error.
    if (result.ok || (result.kind === "http" && result.body.error === "already_set")) {
      onStateChange();
      return;
    }
    error = result.kind === "network" ? result.message : describeAuthError(result.body.error, result.status);
  }
</script>

<form onsubmit={handleSubmit}>
  <label>
    Password
    <input
      type="password"
      bind:value={password}
      autocomplete={mode === "setup" ? "new-password" : "current-password"}
      required
    />
  </label>
  {#if mode === "setup"}
    <label>
      Confirm password
      <input type="password" bind:value={confirmPassword} autocomplete="new-password" required />
    </label>
  {/if}
  {#if error}
    <p class="error" aria-live="polite">{error}</p>
  {/if}
  <button type="submit" disabled={submitting}>
    {#if submitting}
      {mode === "setup" ? "Setting password..." : "Logging in..."}
    {:else}
      {mode === "setup" ? "Set password" : "Log in"}
    {/if}
  </button>
</form>

<style>
  form {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }

  label {
    display: flex;
    flex-direction: column;
    gap: 0.375rem;
    font-size: 0.875rem;
    color: #374151;
  }

  input {
    padding: 0.5rem 0.625rem;
    font-size: 1rem;
    border: 1px solid #d1d5db;
    border-radius: 0.375rem;
    box-sizing: border-box;
  }

  input:focus {
    outline: 2px solid #2563eb;
    outline-offset: -1px;
    border-color: #2563eb;
  }

  .error {
    margin: 0;
    color: #b91c1c;
    font-size: 0.875rem;
  }

  button {
    padding: 0.625rem;
    font-size: 1rem;
    color: white;
    background: #2563eb;
    border: none;
    border-radius: 0.375rem;
    cursor: pointer;
  }

  button:hover:not(:disabled) {
    background: #1d4ed8;
  }

  button:disabled {
    opacity: 0.6;
    cursor: default;
  }
</style>
