#!/usr/bin/env bash
# Decodes a core dump downloaded from the Web UI (Diagnostics, or
# GET /api/diagnostics/coredump) into a backtrace, registers and per-task
# stacks. The ELF must come from the same build that is flashed: pass the saved
# copy of that build's homedeck.elf, or leave the default of the current build.
#
# Usage: tools/decode-coredump.sh <coredump.bin> [path/to/homedeck.elf]
set -euo pipefail

CORE="${1:?usage: tools/decode-coredump.sh <coredump.bin> [homedeck.elf]}"
ELF="${2:-firmware/build/homedeck.elf}"
ROOT="$(git rev-parse --show-toplevel)"

[ -f "$CORE" ] || { echo "$CORE not found." >&2; exit 1; }
[ -f "$ELF" ] || { echo "$ELF not found - build the firmware first or pass the ELF." >&2; exit 1; }

# Paths are made absolute and mounted at the same place inside the container.
CORE="$(realpath "$CORE")"
ELF="$(realpath "$ELF")"

docker run --rm -v "$ROOT:$ROOT" -v "$CORE:$CORE:ro" -v "$ELF:$ELF:ro" -w "$ROOT" \
  espressif/idf:v5.4.3 esp-coredump --chip esp32p4 info_corefile --core "$CORE" --core-format raw "$ELF"
