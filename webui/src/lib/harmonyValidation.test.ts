import { describe, expect, it } from "vitest";
import { hubHostError } from "./harmonyValidation";

describe("hubHostError", () => {
  it("accepts a plain hostname or IP address", () => {
    expect(hubHostError("192.168.1.50")).toBeUndefined();
    expect(hubHostError("harmony-hub.local")).toBeUndefined();
  });

  it("rejects a value with a scheme prefix", () => {
    expect(hubHostError("http://192.168.1.50")).toBe(
      "Enter a hostname or IP address, not a full URL (remove the http:// prefix)",
    );
  });

  it("rejects a value containing whitespace", () => {
    expect(hubHostError("192.168.1.50 ")).toBe("Hub address can't contain whitespace");
    expect(hubHostError("192.168 1.50")).toBe("Hub address can't contain whitespace");
  });

  it("rejects a value containing a path", () => {
    expect(hubHostError("192.168.1.50/setup")).toBe("Hub address can't contain a path");
  });

  it("checks scheme before path, so a full URL gets the scheme error", () => {
    expect(hubHostError("http://192.168.1.50/setup")).toBe(
      "Enter a hostname or IP address, not a full URL (remove the http:// prefix)",
    );
  });

  it("rejects non-ASCII characters, matching the server-side IsValidHubHost() check", () => {
    // An accented character - not whitespace, not a path separator, so
    // it falls through to this check specifically (unlike U+00A0 NBSP,
    // which the whitespace check above already catches via \s).
    expect(hubHostError("héllo.local")).toBe(
      "Hub address must be plain ASCII (no accented or non-Latin characters)",
    );
  });

  it("rejects non-whitespace control characters, matching the server-side check", () => {
    // U+0001 - not whitespace, not a path separator, so it falls through
    // to this check specifically.
    expect(hubHostError("192.168.1.50")).toBe("Hub address can't contain control characters");
  });

  it("rejects #, ?, and @, matching the server-side IsValidHubHost() check", () => {
    // All three pass every other check but change what a URL parser
    // treats as the actual host/port instead of just failing to connect
    // (M3 exit review pass 7).
    expect(hubHostError("realhost#fragment")).toBe("Hub address can't contain #, ?, or @");
    expect(hubHostError("realhost?query=1")).toBe("Hub address can't contain #, ?, or @");
    expect(hubHostError("user@realhost")).toBe("Hub address can't contain #, ?, or @");
  });

  it("accepts an empty value - saving an empty hub address un-configures Harmony", () => {
    // Matches the backend's IsValidHubHost("") test - empty isn't this
    // function's concern, since HarmonySettings.svelte's own save flow
    // treats it as "disconnect the configured hub," not a malformed
    // address (M3 exit review pass 7).
    expect(hubHostError("")).toBeUndefined();
  });
});
