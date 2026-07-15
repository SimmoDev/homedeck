# firmware/

The ESP-IDF firmware project targeting the M5Stack Tab5 (ESP32-P4).

The project scaffold exists and builds — confirmed via
`idf.py set-target esp32p4 build` (see
[DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the exact command),
producing a real `homedeck.bin`. It's the bare ESP-IDF template
(`app_main()` does nothing yet) — no Tab5 boot, display, touch, or any
other on-device bring-up has been done, since that needs real hardware,
not just the toolchain. See [docs/roadmap.md](../docs/roadmap.md) (M1) for
what's next.

The portable Core/UI/module source this will eventually link against is
shared with [../simulator/](../simulator/) — see
[docs/architecture/simulator.md](../docs/architecture/simulator.md). None
of that exists yet; `main/homedeck.c` is still just the unmodified
template.
