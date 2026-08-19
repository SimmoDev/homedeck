// Pure validation/error-mapping logic pulled out of PasswordForm.svelte
// so it's unit-testable without a component-testing stack (Testing
// Library + jsdom) - not yet justified for the rest of the Web UI,
// which is still thin fetch-and-render (see
// docs/architecture/web-ui.md#status).

export const kMinPasswordLength = 8; // matches AdminAuthService::kMinPasswordLength
export const kMaxPasswordLength = 256; // matches AdminAuthService::kMaxPasswordLength

// AdminAuthService::kMinPasswordLength/kMaxPasswordLength bound
// password->size() - a std::string holding the request body's raw UTF-8
// bytes, not a character count. password.length here is JS's own UTF-16
// code-unit count, which under-counts every non-ASCII character (most
// visibly astral-plane characters like emoji, counted as 2 code units
// but 4+ UTF-8 bytes) - a password that reads as within-bounds
// client-side could still be rejected server-side. TextEncoder gives
// the same UTF-8 byte count the server actually checks.
function utf8ByteLength(text: string): number {
  return new TextEncoder().encode(text).length;
}

// Client-side mirror of AdminAuthService's own checks, applied before a
// network round trip - the server remains authoritative regardless (see
// describeAuthError's password_too_short/password_too_long cases, still
// reachable if this is ever out of sync).
export function validateSetupPassword(password: string, confirmPassword: string): string | undefined {
  const byteLength = utf8ByteLength(password);
  if (byteLength < kMinPasswordLength) {
    return `Password must be at least ${kMinPasswordLength} characters.`;
  }
  if (byteLength > kMaxPasswordLength) {
    return `Password must be at most ${kMaxPasswordLength} characters.`;
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
    case "password_too_long":
      return `Password must be at most ${kMaxPasswordLength} characters.`;
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
