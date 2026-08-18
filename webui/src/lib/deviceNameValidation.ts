// Pure validation logic pulled out of DeviceNameSettings.svelte so it's
// unit-testable without a component-testing stack - same reasoning as
// harmonyValidation.ts/passwordValidation.ts. Mirrors the backend's
// IsValidHostnameLabel() (firmware/main/homedeck.cpp): RFC 1035/6763
// label rules - non-empty, at most 63 characters, alphanumeric or hyphen
// only, no leading/trailing hyphen. The simulator's DeviceNameValidateFn
// is omitted entirely (see docs/decisions/ADR-0023-settings-backup-api.md),
// so this check is a UX nicety independent of which backend is running,
// not something the simulator itself enforces server-side.
export function deviceNameError(value: string): string | undefined {
  if (value.length === 0) {
    return "Device name can't be empty";
  }
  if (value.length > 63) {
    return "Device name can't be longer than 63 characters";
  }
  if (value.startsWith("-") || value.endsWith("-")) {
    return "Device name can't start or end with a hyphen";
  }
  if (!/^[A-Za-z0-9-]+$/.test(value)) {
    return "Device name can only contain letters, numbers, and hyphens";
  }
  return undefined;
}
