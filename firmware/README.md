# firmware/

The ESP-IDF firmware project targeting the M5Stack Tab5 (ESP32-P4).

Confirmed on real hardware (a K145 reference unit — see
[docs/architecture/hardware.md](../docs/architecture/hardware.md#on-device-dashboard)):
the real dashboard runs live, not a bring-up placeholder — `main/homedeck.cpp`
boots, initializes the display and touch via
[`espressif/m5stack_tab5`](https://components.espressif.com/components/espressif/m5stack_tab5)
(Espressif's official BSP — no M5Unified/M5GFX, which has a confirmed
crash on this chip under ESP-IDF, see hardware.md), then loads the actual
`EventBus`/`Clock`/`DashboardScreen` reused directly from
[../src/](../src/) — a live ticking clock and a real (not mocked) battery
percentage, both sourced from actual hardware (the RX8130CE RTC and
INA226 power monitor via `src/platform/firmware/`). See
[DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the exact
build/flash procedure — note the Docker mount now covers the whole repo
root, not just `firmware/`.

Not yet done: Navigation, the persistent home affordance, and any second
screen — the dashboard is currently loaded directly as the only screen.
Touch is fully wired into LVGL (confirmed via a real on-screen handler
during bring-up) but nothing in the current app actually uses it yet,
since there's only one screen to navigate from. See
[docs/roadmap.md](../docs/roadmap.md) for what's next.
