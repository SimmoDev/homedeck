// Pure host-string validation shared by harmonyValidation.ts and
// kodiValidation.ts - both build the entered value directly into a URL
// authority (`http://<value>:8088/...` for Harmony,
// `ws://<value>:9090/jsonrpc` for Kodi), so a scheme prefix, whitespace,
// control byte, path, `#`/`?`/`@`, non-ASCII byte, or bare IPv6 colon
// all change *which* URL gets requested rather than merely failing to
// connect the same opaque way an unreachable address does. Caught here,
// before the value is ever saved. Mirrors the backend's
// HasUnsafeHostChars() (src/core/host_validation.h).
//
// `label` is the noun used in each message ("Hub" / "Kodi"); `schemeHint`
// is the scheme(s) to name in the "remove the ... prefix" message.
export function hostError(
  value: string,
  label: string,
  schemeHint: string,
): string | undefined {
  if (value.includes("://")) {
    return `Enter a hostname or IP address, not a full URL (remove the ${schemeHint} prefix)`;
  }
  if (/\s/.test(value)) {
    return `${label} address can't contain whitespace`;
  }
  // Non-whitespace control characters (e.g. U+0001) build the same kind
  // of malformed URL via paste/scripted entry - \s above already covers
  // tab/newline/carriage-return.
  if (/[\x00-\x1f\x7f]/.test(value)) {
    return `${label} address can't contain control characters`;
  }
  if (value.includes("/")) {
    return `${label} address can't contain a path`;
  }
  if (/[#?@]/.test(value)) {
    return `${label} address can't contain #, ?, or @`;
  }
  if (/[^\x00-\x7F]/.test(value)) {
    return `${label} address must be plain ASCII (no accented or non-Latin characters)`;
  }
  // A bare (unbracketed) IPv6 literal like "::1" would otherwise pass
  // every check above.
  if (value.includes(":")) {
    return `${label} address can't contain a colon`;
  }
  return undefined;
}
