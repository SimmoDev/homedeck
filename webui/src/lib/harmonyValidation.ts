// Pure validation logic pulled out of HarmonySettings.svelte so it's
// unit-testable without a component-testing stack - same reasoning as
// passwordValidation.ts.

// The backend URL-builds this as `http://<value>:8088/...` - a scheme
// prefix, embedded whitespace, or a path all produce a malformed URL
// that fails to connect the same opaque way an unreachable address
// does, with no way to tell the two apart. Caught here instead, before
// it's ever saved.
export function hubHostError(value: string): string | undefined {
  if (value.includes("://")) {
    return "Enter a hostname or IP address, not a full URL (remove the http:// prefix)";
  }
  if (/\s/.test(value)) {
    return "Hub address can't contain whitespace";
  }
  // Non-whitespace control characters (e.g. U+0001) build the same kind
  // of malformed URL the whitespace/path checks exist to catch, just via
  // paste/scripted entry rather than typing - \s above already covers
  // tab/newline/carriage-return.
  if (/[\x00-\x1f\x7f]/.test(value)) {
    return "Hub address can't contain control characters";
  }
  if (value.includes("/")) {
    return "Hub address can't contain a path";
  }
  if (/[^\x00-\x7F]/.test(value)) {
    return "Hub address must be plain ASCII (no accented or non-Latin characters)";
  }
  return undefined;
}
