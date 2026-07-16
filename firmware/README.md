# firmware/

The ESP-IDF firmware project targeting the M5Stack Tab5 (ESP32-P4).

Confirmed on real hardware (a K145 reference unit — see
[docs/architecture/hardware.md](../docs/architecture/hardware.md)): boots
cleanly over USB (`idf.py -p /dev/ttyACM0 flash monitor` — see
[DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the exact
procedure), and the display shows real pixels via
[`espressif/m5stack_tab5`](https://components.espressif.com/components/espressif/m5stack_tab5)
(Espressif's official BSP, pulled as a managed component in
`main/idf_component.yml`) — no M5Unified/M5GFX, which has a confirmed
crash on this chip under ESP-IDF (see hardware.md). The BSP's runtime I2C
probing correctly identified the ST7123 display+touch controller and
initialized touch alongside display in the same pass.

`main/homedeck.c` is still a deliberately minimal bring-up program (chip
info, a solid color fill, a heartbeat loop), not real product code — see
its own header comment. Not yet done: touch events aren't wired into any
input handling (controller init only), and no real UI/LVGL application
exists on-device yet — that's the actual next M1 item, see
[docs/roadmap.md](../docs/roadmap.md).

The portable Core/UI/module source this will eventually link against is
shared with [../simulator/](../simulator/) — see
[docs/architecture/simulator.md](../docs/architecture/simulator.md). None
of that exists yet; `main/homedeck.c` doesn't use it.
