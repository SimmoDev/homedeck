<script lang="ts">
  // Shared by both first-login setup and ordinary login - same field,
  // same submit mechanics, differing only in endpoint/copy and setup's
  // extra confirm-password field (a real safeguard, not decoration:
  // there's no password-recovery flow yet, so a setup typo is otherwise
  // unrecoverable without an NVS erase - see ADR-0007's accepted-risk
  // note).
  interface Props {
    mode: "setup" | "login";
    onStateChange: () => void;
  }
  let { mode, onStateChange }: Props = $props();

  const kMinPasswordLength = 8; // matches AdminAuthService's kMinPasswordLength

  let password = $state("");
  let confirmPassword = $state("");
  let error: string | undefined = $state(undefined);
  let submitting = $state(false);

  function DescribeError(code: unknown, status: number): string {
    switch (code) {
      case "password_too_short":
        return `Password must be at least ${kMinPasswordLength} characters.`;
      case "invalid_credentials":
        return "Incorrect password.";
      case "invalid_request":
        return "Invalid request.";
      case "storage_write_failed":
        return "Could not save the password - try again.";
      default:
        return `Request failed (${status}).`;
    }
  }

  async function handleSubmit(event: SubmitEvent) {
    event.preventDefault();
    error = undefined;

    if (mode === "setup") {
      if (password.length < kMinPasswordLength) {
        error = `Password must be at least ${kMinPasswordLength} characters.`;
        return;
      }
      if (password !== confirmPassword) {
        error = "Passwords do not match.";
        return;
      }
    }

    submitting = true;
    try {
      const response = await fetch(mode === "setup" ? "/api/auth/setup" : "/api/auth/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ password }),
      });
      const body = await response.json().catch(() => ({}));
      // already_set means another request won first (the real race
      // ADR-0007 accepts) - the password itself may now be wrong, but
      // the state genuinely changed, so re-checking status is still the
      // right move rather than treating this as this form's own error.
      if (response.ok || body.error === "already_set") {
        onStateChange();
        return;
      }
      error = DescribeError(body.error, response.status);
    } catch (err) {
      error = String(err);
    } finally {
      submitting = false;
    }
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
    <p class="error">{error}</p>
  {/if}
  <button type="submit" disabled={submitting}>
    {mode === "setup" ? "Set password" : "Log in"}
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
