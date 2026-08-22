import { describe, expect, it } from "vitest";
import { tripGuard } from "./guardedAction";

describe("tripGuard", () => {
  it("marks busy and returns false when not already busy", () => {
    let busy = false;
    const tripped = tripGuard(
      () => busy,
      () => (busy = true),
    );
    expect(tripped).toBe(false);
    expect(busy).toBe(true);
  });

  it("returns true and does not call markBusy when already busy", () => {
    let markBusyCalls = 0;
    const tripped = tripGuard(
      () => true,
      () => markBusyCalls++,
    );
    expect(tripped).toBe(true);
    expect(markBusyCalls).toBe(0);
  });

  it("works with a string state machine's busy value, not just a boolean", () => {
    let state: "idle" | "busy" | "done" = "idle";
    const tripped = tripGuard(
      () => state === "busy",
      () => (state = "busy"),
    );
    expect(tripped).toBe(false);
    expect(state).toBe("busy");
  });
});
