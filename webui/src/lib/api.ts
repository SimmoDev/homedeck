// Shared fetch/JSON helpers - every page-level component was hand-
// rolling the same "fetch, check response.ok, parse JSON" sequence for
// its GET loads (identical down to the "Request failed: ${status}"
// wording), and the same untyped `.catch(() => ({}))` error-body parse
// for POST failures.

// The shape of this server's own JSON error responses (e.g.
// core/settings_routes.cpp's {"error":"invalid_value"} bodies) - not
// exhaustive across every endpoint's exact error codes, but every
// caller here only ever reads .error, so this replaces what was
// previously an implicit `any`.
export interface ApiErrorBody {
  error?: string;
  field?: string;
}

// Non-JSON or empty bodies (a raw error from a proxy/crash, not this
// server's own JSON error responses) resolve to {} rather than
// throwing.
export async function readErrorBody(response: Response): Promise<ApiErrorBody> {
  return (await response.json().catch(() => ({}))) as ApiErrorBody;
}

export type LoadResult<T> = { data: T; error?: undefined } | { data?: undefined; error: string };

// Used for GET loads - the fetch/response.ok/json() sequence nearly
// every load()-style function repeated. Network failures (fetch()
// itself throwing) and non-ok HTTP responses both resolve to
// {error: string} rather than throwing, so callers don't need their
// own try/catch.
export async function loadJson<T>(url: string): Promise<LoadResult<T>> {
  try {
    const response = await fetch(url);
    if (!response.ok) {
      return { error: `Request failed: ${response.status}` };
    }
    return { data: (await response.json()) as T };
  } catch (err) {
    return { error: String(err) };
  }
}
