import { describe, expect, it } from "vitest";
import { deviceNameError } from "./deviceNameValidation";

describe("deviceNameError", () => {
  it("accepts a plain alphanumeric-with-hyphens name", () => {
    expect(deviceNameError("living-room")).toBeUndefined();
    expect(deviceNameError("homedeck2")).toBeUndefined();
  });

  it("rejects an empty value", () => {
    expect(deviceNameError("")).toBe("Device name can't be empty");
  });

  it("rejects a value over 63 characters, matching IsValidHostnameLabel()", () => {
    expect(deviceNameError("a".repeat(64))).toBe("Device name can't be longer than 63 characters");
    expect(deviceNameError("a".repeat(63))).toBeUndefined();
  });

  it("rejects a leading or trailing hyphen", () => {
    expect(deviceNameError("-living-room")).toBe("Device name can't start or end with a hyphen");
    expect(deviceNameError("living-room-")).toBe("Device name can't start or end with a hyphen");
  });

  it("rejects characters other than letters, numbers, and hyphens", () => {
    expect(deviceNameError("living room")).toBe("Device name can only contain letters, numbers, and hyphens");
    expect(deviceNameError("living_room")).toBe("Device name can only contain letters, numbers, and hyphens");
    expect(deviceNameError("café")).toBe("Device name can only contain letters, numbers, and hyphens");
  });
});
