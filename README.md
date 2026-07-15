# HomeDeck

HomeDeck is a battery-powered handheld smart home controller built around
the [M5Stack Tab5 Kit](https://docs.m5stack.com/en/core/Tab5). It's
designed to replace a Logitech Harmony Hub remote, then grow into a
general-purpose handheld controller for media systems, smart home devices,
home monitoring, and personal dashboards.

Harmony is the first supported integration — not the identity of the
project. See [ADR-0001](docs/decisions/ADR-0001-project-vision.md) for the
full project vision.

## Goals

- A polished, consumer-quality handheld smart home control device — not a
  hobby project with a screen.
- A complete, reliable replacement for a Harmony Hub remote as the first
  milestone that actually matters to a user.
- A modular architecture that lets new integrations (Kodi, Uptime Kuma, Home
  Assistant, and beyond) be added without compromising the reliability of
  what's already there.
- Local-first control: LAN and direct device communication preferred over
  cloud dependencies wherever possible.

## Supported hardware

- **M5Stack Tab5 Kit** — the primary and currently only supported target.
  Application processor is an ESP32-P4, which has no radio; Wi-Fi/BT run
  through an onboard ESP32-C6-MINI-1U wireless co-processor — see
  [docs/architecture/hardware.md](docs/architecture/hardware.md) for the full
  confirmed hardware reference. Capacitive touchscreen, display, IMU, RTC,
  battery monitoring, speaker, microphones, USB-C, and microSD are all
  expected to be used by the firmware.

No other hardware is supported today. The architecture avoids unnecessary
coupling to Tab5-specific quirks so that future hardware variants remain
possible, but none is planned or committed to.

## Current status

**Milestone M0 — Foundation.** The repository structure, architecture
documentation, and initial ADRs have just been established. No firmware
implementation exists yet. See [docs/roadmap.md](docs/roadmap.md) for the
full milestone plan and the architectural decisions index.

## Architecture

HomeDeck is organized into four layers:

```
UI
  ↓
HomeDeck Core
  ↓
Apps / Modules
  ↓
External Services / Devices
```

- **UI** — a Touch UI (on-device, LVGL) for everyday use, and a Web
  Management UI (browser-based) for administration.
- **HomeDeck Core** — shared services: lifecycle, navigation, dashboard,
  widgets, notifications, event bus, configuration, storage, networking,
  logging, diagnostics, OTA, power management, time/date, and weather.
- **Apps / Modules** — isolated integrations (Harmony, Kodi, Uptime Kuma,
  Home Assistant, ...) that use Core services and appear to the user as
  Apps.
- **External Services / Devices** — the actual hardware/services a module
  talks to (a Harmony Hub, a Kodi instance, etc.).

The full architecture is documented in
[docs/architecture/](docs/architecture/), starting with
[overview.md](docs/architecture/overview.md). Significant architectural
decisions are recorded as ADRs in
[docs/decisions/](docs/decisions/).

## Repository structure

```
HomeDeck/
├── docs/            architecture, ADRs, roadmap
├── firmware/        ESP-IDF firmware (Tab5 target)
├── simulator/       desktop simulator build
├── webui/           Web Management UI frontend
├── hardware/        physical accessory/enclosure design work
├── tools/           supporting developer tooling
└── tests/           test suites
```

`firmware/` and `simulator/` share the same portable Core/UI/module source;
they differ only in which hardware-facing implementation they're built
against. See [docs/architecture/simulator.md](docs/architecture/simulator.md).

## License

MIT — see [LICENSE](LICENSE) and
[ADR-0001](docs/decisions/ADR-0001-project-vision.md#decision-license).

## Getting involved

HomeDeck is early-stage — see [docs/roadmap.md](docs/roadmap.md) for the
current milestone. For development setup, see
[DEVELOPMENT.md](DEVELOPMENT.md).
