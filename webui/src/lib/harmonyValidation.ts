// Pure validation logic pulled out of HarmonySettings.svelte so it's
// unit-testable without a component-testing stack - same reasoning as
// passwordValidation.ts.

import { hostError } from "./hostValidation";

// The backend URL-builds this as `http://<value>:8088/...`. The byte
// classes rejected here, and why, are in hostValidation.ts; this just
// pins the Harmony wording. Matches the server-side IsValidHubHost().
export function hubHostError(value: string): string | undefined {
  return hostError(value, "Hub", "http://");
}
