// Pure validation/error-mapping logic pulled out of PasswordForm.svelte
// so it's unit-testable without a component-testing stack (Testing
// Library + jsdom) - not yet justified for the rest of the Web UI,
// which is still thin fetch-and-render (see
// docs/architecture/web-ui.md#status).

export const kMinPasswordLength = 8; // matches AdminAuthService::kMinPasswordLength

// Client-side mirror of AdminAuthService's own checks, applied before a
// network round trip - the server remains authoritative regardless (see
// describeAuthError's password_too_short case, still reachable if this
// is ever out of sync).
export function validateSetupPassword(password: string, confirmPassword: string): string | undefined {
  if (password.length < kMinPasswordLength) {
    return `Password must be at least ${kMinPasswordLength} characters.`;
  }
  if (password !== confirmPassword) {
    return "Passwords do not match.";
  }
  return undefined;
}

export function describeAuthError(code: unknown, status: number): string {
  switch (code) {
    case "password_too_short":
      return `Password must be at least ${kMinPasswordLength} characters.`;
    case "invalid_credentials":
      return "Incorrect password.";
    case "too_many_attempts":
      return "Too many failed attempts. Try again in a minute.";
    case "invalid_request":
      return "Invalid request.";
    case "storage_write_failed":
      return "Could not save the password - try again.";
    default:
      return `Request failed (${status}).`;
  }
}
