# ADR-0001: Project Vision and Scope

## Status

Accepted

## Context

Logitech Harmony Hub remotes have been end-of-life since 2021, with Logitech
progressively winding down the cloud services that the Harmony ecosystem
depended on for setup and account management. Existing Harmony Hub owners are
left with hardware that still works but has an increasingly uncertain future
and no actively developed client.

At the same time, "smart home controller" as a product category is
underserved by good dedicated hardware. Most solutions are either phone apps
(inconvenient, easy to misplace, competing with notifications) or wall panels
(fixed, expensive, single-vendor). The M5Stack Tab5 provides an affordable,
battery-powered, touch-enabled ESP32-P4 platform that is well suited to a
handheld controller role.

HomeDeck needs a clear, durable statement of what it is and is not, so that
implementation decisions later in the project have a stable reference point.

## Decision

HomeDeck is a battery-powered handheld smart home controller built on the
M5Stack Tab5. It is a general-purpose control device for media systems, smart
home devices, home monitoring, and personal dashboards.

Harmony Hub replacement is the **first integration**, chosen because it
addresses an immediate, real need (Harmony's decline) and because remote
control is a well-understood problem that exercises the core architecture
(devices, activities, commands, event-driven state). Harmony is not the
identity of the project — it is the first proof of the architecture.

The product bar is "polished, consumer-quality handheld device," not "hobby
project with a screen." This has direct consequences for scope discipline:
features are only added once they can be delivered to a reliable, well
finished standard.

## Decision: License

CLAUDE.md establishes HomeDeck as open-source but did not specify a license.

**Options considered:**

| Option | Characteristics |
|---|---|
| MIT | Maximally permissive, minimal text, well understood, no patent clause |
| Apache-2.0 | Permissive, includes explicit patent grant and contribution terms — relevant given hardware/firmware involvement and potential third-party contributions |
| GPL-3.0 | Copyleft; would require derivative firmware to remain open source, but complicates commercial/vendor reuse (e.g. someone building a HomeDeck-based product) and mixing with permissively-licensed dependencies (LVGL is MIT, M5Unified is MIT — the dependency landscape at decision time; display/touch specifically later moved to `espressif/m5stack_tab5`, also permissively licensed, per [ADR-0014](ADR-0014-hardware-support-library.md)) |

**Decided: MIT.** Simplicity and instant recognizability outweighed
Apache-2.0's patent grant for a project with no near-term commercial or
third-party hardware-contribution angle. It remains fully compatible with
LVGL's MIT licensing. The actual hardware BSP — `espressif/m5stack_tab5`
(not M5Unified, for display/touch specifically; see
[ADR-0014](ADR-0014-hardware-support-library.md)) — is permissively
licensed too. See the root [LICENSE](../../LICENSE) file.

## Consequences

- Milestone M3 (Harmony) is treated as the first true product validation
  point. HomeDeck is not considered viable until it fully replaces a Harmony
  remote's day-to-day usage.
- Every module after Harmony (Kodi, Uptime Kuma, Home Assistant, ...) must
  fit the same architectural contract established for Harmony. No module gets
  a special-case integration path.
- Scope creep is an explicit risk given the breadth of "general-purpose smart
  home controller." The roadmap (see [roadmap.md](../roadmap.md)) and the
  scope control section of CLAUDE.md are the guardrails against this.
- The MIT `LICENSE` file at the repo root reflects the license decision
  above; any dependency added later should be checked for MIT compatibility
  before being pulled in, not after.
