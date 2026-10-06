// Display formatting for GET /api/diagnostics' memory figures, kept out of
// CrashDiagnostics.svelte so it is unit-testable.

const kib = 1024;
const mib = 1024 * 1024;

export function formatKib(bytes: number): string {
  return `${Math.round(bytes / kib)} KiB`;
}

export function formatMib(bytes: number): string {
  return `${(bytes / mib).toFixed(1)} MiB`;
}
