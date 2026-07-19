import { describe, expect, it } from "vitest";
import { describeAuthError, kMinPasswordLength, validateSetupPassword } from "./passwordValidation";

describe("validateSetupPassword", () => {
  it("rejects passwords under the minimum length", () => {
    const short = "a".repeat(kMinPasswordLength - 1);
    expect(validateSetupPassword(short, short)).toBe(
      `Password must be at least ${kMinPasswordLength} characters.`,
    );
  });

  it("rejects mismatched passwords that are otherwise long enough", () => {
    const long = "a".repeat(kMinPasswordLength);
    expect(validateSetupPassword(long, long + "x")).toBe("Passwords do not match.");
  });

  it("accepts a long enough, matching password", () => {
    const long = "a".repeat(kMinPasswordLength);
    expect(validateSetupPassword(long, long)).toBeUndefined();
  });

  it("checks length before match, so a short mismatched password gets the length error", () => {
    expect(validateSetupPassword("short", "different")).toBe(
      `Password must be at least ${kMinPasswordLength} characters.`,
    );
  });
});

describe("describeAuthError", () => {
  it("maps known error codes to specific messages", () => {
    expect(describeAuthError("invalid_credentials", 401)).toBe("Incorrect password.");
    expect(describeAuthError("invalid_request", 400)).toBe("Invalid request.");
    expect(describeAuthError("password_too_short", 400)).toBe(
      `Password must be at least ${kMinPasswordLength} characters.`,
    );
    expect(describeAuthError("storage_write_failed", 500)).toBe("Could not save the password - try again.");
  });

  it("falls back to a generic message carrying the status code for unknown codes", () => {
    expect(describeAuthError("something_unexpected", 418)).toBe("Request failed (418).");
    expect(describeAuthError(undefined, 500)).toBe("Request failed (500).");
  });
});
