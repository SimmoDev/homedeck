import { describe, expect, it } from "vitest";
import { kodiHostError } from "./kodiValidation";

describe("kodiHostError", () => {
  it("accepts a plain hostname or IP address, and empty (use discovery)", () => {
    expect(kodiHostError("10.0.30.20")).toBeUndefined();
    expect(kodiHostError("kodi.local")).toBeUndefined();
    expect(kodiHostError("")).toBeUndefined();
  });

  it("rejects a scheme prefix", () => {
    expect(kodiHostError("ws://10.0.30.20")).toBe(
      "Enter a hostname or IP address, not a full URL (remove the ws:// or http:// prefix)",
    );
  });

  it("rejects whitespace, a path, and structural URL characters", () => {
    expect(kodiHostError("10.0.30.20 ")).toBe("Kodi address can't contain whitespace");
    expect(kodiHostError("10.0.30.20/jsonrpc")).toBe("Kodi address can't contain a path");
    expect(kodiHostError("host#frag")).toBe("Kodi address can't contain #, ?, or @");
    expect(kodiHostError("user@host")).toBe("Kodi address can't contain #, ?, or @");
  });

  it("rejects non-ASCII and a bare IPv6 literal, matching the server-side IsValidKodiHost()", () => {
    expect(kodiHostError("héllo.local")).toBe(
      "Kodi address must be plain ASCII (no accented or non-Latin characters)",
    );
    expect(kodiHostError("fe80::1")).toBe("Kodi address can't contain a colon");
  });
});
