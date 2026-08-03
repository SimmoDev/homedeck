// Shared fetch/JSON helpers - every page-level component was hand-
// rolling the same "fetch, check response.ok, parse JSON" sequence for
// its GET loads (identical down to the "Request failed: ${status}"
// wording), and the same untyped `.catch(() => ({}))` error-body parse
// for POST failures.

// The shape of this server's own JSON error responses (e.g.
// core/settings_routes.cpp's {"error":"invalid_value"} bodies) - not
// exhaustive across every endpoint's exact error codes, so this
// replaces what was previously an implicit `any`. Most callers only
// read .error; reason is specific to core/ota_routes.cpp's
// {"error":"gate_closed","reason":...} body.
export interface ApiErrorBody {
  error?: string;
  field?: string;
  reason?: string;
}

// Non-JSON or empty bodies (a raw error from a proxy/crash, not this
// server's own JSON error responses) resolve to {} rather than
// throwing.
export async function readErrorBody(response: Response): Promise<ApiErrorBody> {
  notifyIfSessionExpired(response.status);
  return (await response.json().catch(() => ({}))) as ApiErrorBody;
}

// Registered once by App.svelte - the single place that decides whether
// the authenticated view should be showing at all. Every helper below
// that sees a 401 (AdminAuthService::RequireAuth's "unauthenticated"
// response - a lapsed 24h session, or the in-memory session map clearing
// across an OTA reboot) calls this, so a stale session drops the whole
// app back to the login screen instead of each component separately
// dead-ending on its own "...failed: 401" message.
let onSessionExpired: (() => void) | undefined;

export function setSessionExpiredHandler(handler: () => void): void {
  onSessionExpired = handler;
}

// Exported (not just used internally by readErrorBody()/loadJson()
// above) for the one caller that can't produce a fetch Response to pass
// through those helpers: Ota.svelte's upload(), which deliberately uses
// XMLHttpRequest instead of fetch for upload-progress reporting.
export function notifyIfSessionExpired(status: number): void {
  if (status === 401) {
    onSessionExpired?.();
  }
}

export type LoadResult<T> = { data: T; error?: undefined } | { data?: undefined; error: string };

// One entry from GET /api/settings (core/settings_routes.cpp's
// EntryToJson) - the shape every settings-backed component reads.
export interface SettingEntry {
  module: string;
  key: string;
  value: string;
  schemaVersion: number;
}

// Looks up one setting by (module, key) among a GET /api/settings
// response - the "find the one value this component cares about among
// everything the generic endpoint returned" step every settings-backed
// component needs at least once (DeviceNameSettings.svelte,
// WeatherSettings.svelte).
export function findSetting(entries: SettingEntry[], module: string, key: string): string | undefined {
  return entries.find((entry) => entry.module === module && entry.key === key)?.value;
}

// The battery/power fields both core/diagnostics_routes.cpp and
// core/ota_routes.cpp report identically from the same BatteryReader -
// Diagnostics.svelte and Ota.svelte each extend this into their own
// wider status shape rather than redeclaring these three fields.
export interface BatteryStatus {
  batteryPercent: number;
  externalPowerConnected: boolean;
  batteryPresent: boolean;
}

// Used for GET loads - the fetch/response.ok/json() sequence nearly
// every load()-style function repeated. Network failures (fetch()
// itself throwing) and non-ok HTTP responses both resolve to
// {error: string} rather than throwing, so callers don't need their
// own try/catch.
export async function loadJson<T>(url: string): Promise<LoadResult<T>> {
  try {
    const response = await fetch(url);
    if (!response.ok) {
      notifyIfSessionExpired(response.status);
      return { error: `Request failed: ${response.status}` };
    }
    return { data: (await response.json()) as T };
  } catch (err) {
    return { error: String(err) };
  }
}

// Used for POST actions - the fetch/response.ok/readErrorBody()/catch
// shell nearly every save()/action-style function repeated, each with
// its own copy of the same three branches. Unlike loadJson()'s single
// error string, callers here need the parsed ApiErrorBody back (to map
// specific error codes to their own message, e.g. "invalid_value" ->
// "Not a valid device name.") and need to tell an HTTP-level failure
// apart from a network-level one (some callers, e.g. Ota.svelte's
// reboot(), deliberately treat a network failure as a likely-benign
// "the device probably already rebooted" case rather than a real
// error) - hence the discriminated result instead of a single error
// string.
export type PostJsonResult<T> =
  | { ok: true; data: T }
  | { ok: false; kind: "http"; status: number; body: ApiErrorBody }
  | { ok: false; kind: "network"; message: string };

// body is sent as-is if already a string (e.g. BackupSettings.svelte's
// restore(), which already has the raw JSON file contents), JSON-
// stringified otherwise, or omitted entirely (no body, no Content-Type
// header) if undefined - matching the three shapes callers need.
export async function postJson<T = unknown>(url: string, body?: unknown): Promise<PostJsonResult<T>> {
  try {
    const init: RequestInit = { method: "POST" };
    if (body !== undefined) {
      init.headers = { "Content-Type": "application/json" };
      init.body = typeof body === "string" ? body : JSON.stringify(body);
    }
    const response = await fetch(url, init);
    if (!response.ok) {
      return { ok: false, kind: "http", status: response.status, body: await readErrorBody(response) };
    }
    const data = (await response.json().catch(() => undefined)) as T;
    return { ok: true, data };
  } catch (err) {
    return { ok: false, kind: "network", message: String(err) };
  }
}

// Triggers a browser file download via fetch + Blob rather than a plain
// <a href download> - a bare anchor navigates directly to the URL
// outside of JS, so it has no way to notice a 401 (or any other
// failure); a stale session would just download a small JSON error body
// under the intended filename instead of dropping back to login the way
// every other authenticated action in this app does via
// notifyIfSessionExpired()/readErrorBody().
export async function downloadFile(url: string, filename: string): Promise<{ error?: string }> {
  try {
    const response = await fetch(url);
    if (!response.ok) {
      const body = await readErrorBody(response);
      return { error: body.error ?? `Download failed: ${response.status}` };
    }
    const blob = await response.blob();
    const objectUrl = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = objectUrl;
    link.download = filename;
    link.click();
    URL.revokeObjectURL(objectUrl);
    return {};
  } catch (err) {
    return { error: String(err) };
  }
}
