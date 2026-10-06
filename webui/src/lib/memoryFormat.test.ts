import { describe, expect, it } from "vitest";
import { formatKib, formatMib } from "./memoryFormat";

describe("formatKib", () => {
  it("rounds to whole kibibytes", () => {
    expect(formatKib(141531)).toBe("138 KiB");
    expect(formatKib(0)).toBe("0 KiB");
  });
});

describe("formatMib", () => {
  it("shows one decimal place", () => {
    expect(formatMib(28280108)).toBe("27.0 MiB");
    expect(formatMib(1572864)).toBe("1.5 MiB");
  });
});
