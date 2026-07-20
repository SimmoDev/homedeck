#!/usr/bin/env bash
# Opens an interactive serial monitor against real Tab5 hardware. Ctrl+]
# exits; Ctrl+C does not, since the monitor forwards most keystrokes to
# the device instead of treating them as terminal control - see
# DEVELOPMENT.md's ESP-IDF setup section.
#
# Usage: tools/monitor.sh [serial-port]  (default: /dev/ttyACM0)
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
IDF_IMAGE="espressif/idf:v5.4.3"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker run --rm -it -v "$REPO_ROOT:/project" -w /project/firmware \
  --device="$PORT" \
  "$IDF_IMAGE" idf.py -p "$PORT" monitor
