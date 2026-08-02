import { afterEach, describe, expect, it, vi } from "vitest";
import { loadJson, readErrorBody, setSessionExpiredHandler } from "./api";

describe("session-expiry notification", () => {
  afterEach(() => {
    // Every helper below routes through the single handler App.svelte
    // registers - reset it after each test so one test's spy can't leak
    // into the next.
    setSessionExpiredHandler(() => {});
  });

  it("readErrorBody notifies the registered handler on a 401", async () => {
    const onExpired = vi.fn();
    setSessionExpiredHandler(onExpired);

    await readErrorBody(new Response(JSON.stringify({ error: "unauthenticated" }), { status: 401 }));

    expect(onExpired).toHaveBeenCalledOnce();
  });

  it("readErrorBody does not notify on a non-401 error", async () => {
    const onExpired = vi.fn();
    setSessionExpiredHandler(onExpired);

    await readErrorBody(new Response(JSON.stringify({ error: "invalid_value" }), { status: 400 }));

    expect(onExpired).not.toHaveBeenCalled();
  });

  it("loadJson notifies the registered handler on a 401", async () => {
    const onExpired = vi.fn();
    setSessionExpiredHandler(onExpired);
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(new Response(null, { status: 401 })),
    );

    await loadJson("/api/settings");

    expect(onExpired).toHaveBeenCalledOnce();
  });
});

describe("readErrorBody", () => {
  it("parses a well-formed JSON error body", async () => {
    const body = await readErrorBody(
      new Response(JSON.stringify({ error: "gate_closed", reason: "battery too low" }), { status: 409 }),
    );
    expect(body).toEqual({ error: "gate_closed", reason: "battery too low" });
  });

  it("falls back to an empty object for a non-JSON body, rather than throwing", async () => {
    const body = await readErrorBody(new Response("not json", { status: 500 }));
    expect(body).toEqual({});
  });

  it("falls back to an empty object for an empty body", async () => {
    const body = await readErrorBody(new Response(null, { status: 500 }));
    expect(body).toEqual({});
  });
});

describe("loadJson", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("resolves to the parsed body on a successful response", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(new Response(JSON.stringify({ batteryPercent: 42 }), { status: 200 })),
    );

    const result = await loadJson<{ batteryPercent: number }>("/api/diagnostics/status");

    expect(result).toEqual({ data: { batteryPercent: 42 } });
  });

  it("resolves to an error carrying the status code on a non-ok response", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(null, { status: 503 })));

    const result = await loadJson("/api/diagnostics/status");

    expect(result).toEqual({ error: "Request failed: 503" });
  });

  it("resolves to an error rather than throwing when fetch itself rejects", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new TypeError("Failed to fetch")));

    const result = await loadJson("/api/diagnostics/status");

    expect(result.error).toContain("Failed to fetch");
  });
});
