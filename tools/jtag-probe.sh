#!/usr/bin/env bash
# Checks whether OpenOCD can reach the Tab5's ESP32-P4 over its built-in
# USB-Serial/JTAG controller, with no external probe: connect the USB-C port to
# this machine and run it. Prints the targets OpenOCD examined; it does not halt
# the CPU. Success means GDB can attach with the same configuration.
#
# Usage: tools/jtag-probe.sh
# Needs Docker and read/write access to /dev/bus/usb (a udev rule or sudo).
set -euo pipefail

docker run --rm --device=/dev/bus/usb \
  espressif/idf:v5.4.3 \
  timeout 30 openocd -f board/esp32p4-builtin.cfg -c "init; targets; shutdown"
