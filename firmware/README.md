# firmware/

The ESP-IDF firmware project targeting the M5Stack Tab5 (ESP32-P4).

No implementation exists yet — this milestone is documentation/architecture
only. See [docs/roadmap.md](../docs/roadmap.md) (M1) and
[docs/architecture/overview.md](../docs/architecture/overview.md).

This directory will contain the ESP-IDF project (`CMakeLists.txt`,
`sdkconfig`, `main/`, component code) once M1 implementation begins. The
portable Core/UI/module source it links against is shared with
[../simulator/](../simulator/) — see
[docs/architecture/simulator.md](../docs/architecture/simulator.md).
