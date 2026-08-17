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
});
