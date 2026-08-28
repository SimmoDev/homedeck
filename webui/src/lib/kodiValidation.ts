// Pure validation for the manual Kodi address, pulled out of
// KodiSettings.svelte so it's unit-testable - same shape as
// harmonyValidation.ts (the checks are identical; only the wording
// differs, and both mirror the server-side IsValidKodiHost /
// IsValidHubHost). The backend URL-builds this as
// `ws://<value>:9090/jsonrpc`.
export function kodiHostError(value: string): string | undefined {
  if (value.includes("://")) {
    return "Enter a hostname or IP address, not a full URL (remove the ws:// or http:// prefix)";
  }
  if (/\s/.test(value)) {
    return "Kodi address can't contain whitespace";
  }
  if (/[\x00-\x1f\x7f]/.test(value)) {
    return "Kodi address can't contain control characters";
  }
  if (value.includes("/")) {
    return "Kodi address can't contain a path";
  }
  if (/[#?@]/.test(value)) {
    return "Kodi address can't contain #, ?, or @";
  }
  if (/[^\x00-\x7F]/.test(value)) {
    return "Kodi address must be plain ASCII (no accented or non-Latin characters)";
  }
  if (value.includes(":")) {
    return "Kodi address can't contain a colon";
  }
  return undefined;
}
