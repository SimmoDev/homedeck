# ADR-0009: Touch/Display Controller Detection Strategy

## Status

Accepted — superseded by [ADR-0014](ADR-0014-hardware-support-library.md),
which built the runtime detection this ADR calls for directly into the
hardware support library it chose, rather than the persisted-result/
manual-override design decided below.

## Context

[hardware.md](../architecture/hardware.md#display-and-touch) records that
M5Stack has shipped the Tab5 with three different display/touch driver
configurations across its production life: ILI9881C+GT911 (I2C address
0x14) originally, replaced by an integrated ST7123 driver (I2C address
0x55) from 2025-10-14, replaced again by ST7121 (also 0x55) from
2026-04-28. Units purchased at different times carry different silicon.
The hardware abstraction layer's touch/display implementation needs a
strategy for handling this that doesn't assume a single controller.

## Decision

**Options:**
- Compile-time configuration (a Kconfig option or build flag selecting one
  of the three variants).
- Runtime I2C probing on every boot, no persistence.
- Runtime I2C probing once, persisted via Core's storage service, with a
  manual override available.

**Decided:** the third option, but only after confirming M5Unified/M5GFX's
own Tab5 board support doesn't already handle this — duplicating detection
logic the hardware support library already provides would create two
sources of truth for the same fact. If M5Unified doesn't yet cover it
(plausible, since ST7123/ST7121 are very recent hardware revisions), the
HAL implements probing directly: address `0x14` present indicates GT911;
`0x55` present indicates ST7123 or ST7121, disambiguated by reading a
chip-ID register at that address if one exists (**not yet confirmed —
verify against the datasheet during M1 bring-up**; if no such register
exists and no behavioral difference between ST7123 and ST7121 surfaces
during bring-up, they can be treated as one driver case until a concrete
difference is found). The result is persisted via Core's storage service
so it isn't re-probed on every boot, with a manual override exposed in Web
UI diagnostics for the rare case detection is wrong or ambiguous.

Compile-time configuration was rejected specifically because of OTA: a
single firmware image is meant to update every device regardless of which
hardware revision it shipped with. A compile-time flag would mean either
maintaining separate firmware images per revision (real ongoing OTA
routing complexity) or accepting that an OTA update silently breaks touch/
display on units with the "wrong" compiled-in variant — neither is
acceptable for a feature [CLAUDE.md](../../CLAUDE.md) treats as first-class. Probing on every
boot without persistence was rejected as unnecessary latency for a fact
that never changes on a given physical unit.

## Consequences

- [hardware.md](../architecture/hardware.md#display-and-touch) states the
  resolved approach without repeating this reasoning.
- M1 display/touch bring-up must first check M5Unified/M5GFX's existing
  Tab5 support before writing any new detection code — see
  [ADR-0014](ADR-0014-hardware-support-library.md) for the result of that
  check and its consequences for the detection design below.
- Whether ST7123 and ST7121 need genuinely separate driver paths, or can
  share one, is an open sub-question to resolve during M1 bring-up, not
  before — it depends on hardware facts (a chip-ID register, or a real
  behavioral difference) that aren't confirmed yet.
