import { afterEach, describe, expect, it, vi } from "vitest";
import { downloadFile, loadJson, postJson, readErrorBody, setSessionExpiredHandler } from "./api";

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

describe("postJson", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it("resolves ok with the parsed body on a successful response", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(JSON.stringify({ apSsid: "HomeDeck-ABC" }), { status: 200 })));

    const result = await postJson<{ apSsid: string }>("/api/wifi/reset");

    expect(result).toEqual({ ok: true, data: { apSsid: "HomeDeck-ABC" } });
  });

  it("resolves ok with undefined data when the successful response has no body", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(null, { status: 200 })));

    const result = await postJson("/api/ota/reboot");

    expect(result).toEqual({ ok: true, data: undefined });
  });

  it("JSON-stringifies a non-string body with a JSON content-type header", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(null, { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);

    await postJson("/api/settings", { module: "core", key: "device_name", value: "living-room" });

    expect(fetchMock).toHaveBeenCalledWith("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ module: "core", key: "device_name", value: "living-room" }),
    });
  });

  it("sends a string body as-is, e.g. an already-serialized backup file", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(null, { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);

    await postJson("/api/backup/restore", '{"settings":[]}');

    expect(fetchMock).toHaveBeenCalledWith("/api/backup/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: '{"settings":[]}',
    });
  });

  it("sends no body or content-type header when body is omitted", async () => {
    const fetchMock = vi.fn().mockResolvedValue(new Response(null, { status: 200 }));
    vi.stubGlobal("fetch", fetchMock);

    await postJson("/api/ota/reboot");

    expect(fetchMock).toHaveBeenCalledWith("/api/ota/reboot", { method: "POST" });
  });

  it("resolves to an http-kind error carrying the status and parsed body on a non-ok response", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: "invalid_value" }), { status: 400 })),
    );

    const result = await postJson("/api/settings", { module: "core", key: "device_name", value: "" });

    expect(result).toEqual({ ok: false, kind: "http", status: 400, body: { error: "invalid_value" } });
  });

  it("resolves to a network-kind error rather than throwing when fetch itself rejects", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new TypeError("Failed to fetch")));

    const result = await postJson("/api/ota/reboot");

    if (result.ok || result.kind !== "network") {
      throw new Error("expected postJson to resolve with a network-kind error");
    }
    expect(result.message).toContain("Failed to fetch");
  });

  it("notifies the session-expired handler on a 401, the same as loadJson/readErrorBody", async () => {
    const onExpired = vi.fn();
    setSessionExpiredHandler(onExpired);
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(null, { status: 401 })));

    await postJson("/api/settings", { module: "core", key: "device_name", value: "x" });

    expect(onExpired).toHaveBeenCalledOnce();
    setSessionExpiredHandler(() => {});
  });
});

describe("downloadFile", () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    setSessionExpiredHandler(() => {});
  });

  // No jsdom/happy-dom dependency in this project (see
  // passwordValidation.ts's own comment on why a DOM-testing stack isn't
  // yet justified here) - document/URL are stubbed directly with the
  // minimal shape downloadFile() actually calls, rather than pulling one
  // in for this single function.
  it("creates an object URL, clicks a download link, and revokes the URL on success", async () => {
    const blob = new Blob(["coredump bytes"]);
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(blob, { status: 200 })));
    const createObjectURL = vi.fn().mockReturnValue("blob:fake-url");
    const revokeObjectURL = vi.fn();
    vi.stubGlobal("URL", { createObjectURL, revokeObjectURL });
    const click = vi.fn();
    const fakeLink = { href: "", download: "", click };
    const createElement = vi.fn().mockReturnValue(fakeLink);
    vi.stubGlobal("document", { createElement });

    const result = await downloadFile("/api/diagnostics/coredump", "coredump.bin");

    expect(result).toEqual({});
    expect(createElement).toHaveBeenCalledWith("a");
    expect(fakeLink.download).toBe("coredump.bin");
    expect(fakeLink.href).toBe("blob:fake-url");
    expect(click).toHaveBeenCalledOnce();
    expect(revokeObjectURL).toHaveBeenCalledWith("blob:fake-url");
  });

  it("resolves with the parsed error body's message on a non-ok response, without downloading anything", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn().mockResolvedValue(new Response(JSON.stringify({ error: "unauthenticated" }), { status: 401 })),
    );
    const createElement = vi.fn();
    vi.stubGlobal("document", { createElement });

    const result = await downloadFile("/api/diagnostics/coredump", "coredump.bin");

    expect(result).toEqual({ error: "unauthenticated" });
    expect(createElement).not.toHaveBeenCalled();
  });

  it("notifies the session-expired handler on a 401, the same as every other authenticated call", async () => {
    const onExpired = vi.fn();
    setSessionExpiredHandler(onExpired);
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(null, { status: 401 })));

    await downloadFile("/api/diagnostics/coredump", "coredump.bin");

    expect(onExpired).toHaveBeenCalledOnce();
  });

  it("falls back to a generic message when the error body has no parseable error field", async () => {
    vi.stubGlobal("fetch", vi.fn().mockResolvedValue(new Response(null, { status: 500 })));

    const result = await downloadFile("/api/diagnostics/coredump", "coredump.bin");

    expect(result).toEqual({ error: "Download failed: 500" });
  });

  it("resolves to an error rather than throwing when fetch itself rejects", async () => {
    vi.stubGlobal("fetch", vi.fn().mockRejectedValue(new TypeError("Failed to fetch")));

    const result = await downloadFile("/api/diagnostics/coredump", "coredump.bin");

    expect(result.error).toContain("Failed to fetch");
  });
});
