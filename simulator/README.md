# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

No implementation exists yet. See
[docs/architecture/simulator.md](../docs/architecture/simulator.md) for the
design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision. Planned for M1.
