# firmware/

The ESP-IDF firmware project targeting the M5Stack Tab5 (ESP32-P4).

`main/homedeck.cpp` boots the real dashboard on a K145 reference unit —
display and touch via
[`espressif/m5stack_tab5`](https://components.espressif.com/components/espressif/m5stack_tab5)
(Espressif's official BSP; not M5Unified/M5GFX, see
[hardware.md](../docs/architecture/hardware.md#display-driver-strategy)),
then the portable `EventBus`/`Clock`/`DashboardScreen` and platform
services reused directly from [../src/](../src/). See
[docs/roadmap.md](../docs/roadmap.md) for what's built and what's still
open, and [DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the
build/flash procedure.

Navigation and a second screen (`WifiSetupScreen`) are wired in here the
same as on the simulator — `homedeck.cpp` routes to it via
`GetNavigation().GoTo("wifi-setup")`/`GoHome()` during Wi-Fi setup. The
persistent home affordance isn't visible on hardware yet, not because
it's unimplemented but because `WifiSetupScreen` is the sole screen that
exists so far and is a deliberate exception to it — see
[ui.md](../docs/architecture/ui.md#navigation-model) for why.
