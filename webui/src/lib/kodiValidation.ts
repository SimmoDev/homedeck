// Pure validation for the manual Kodi address, pulled out of
// KodiSettings.svelte so it's unit-testable - same shape as
// harmonyValidation.ts. The backend URL-builds this as
// `ws://<value>:9090/jsonrpc`.

import { hostError } from "./hostValidation";

// Byte classes and rationale live in hostValidation.ts; this just pins
// the Kodi wording. Matches the server-side IsValidKodiHost().
export function kodiHostError(value: string): string | undefined {
  return hostError(value, "Kodi", "ws:// or http://");
}
