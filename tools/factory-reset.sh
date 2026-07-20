#!/usr/bin/env bash
# Erases the Tab5's entire flash - settings, secrets, OTA state, and the
# firmware image itself. The device will not boot again until reflashed
# (tools/flash.sh). Irreversible, so this confirms before proceeding.
#
# Usage: tools/factory-reset.sh [serial-port]  (default: /dev/ttyACM0)
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
IDF_IMAGE="espressif/idf:v5.4.3"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

read -r -p "This erases ALL flash on the device at $PORT, including the running firmware. Type 'erase' to continue: " CONFIRM
if [ "$CONFIRM" != "erase" ]; then
  echo "Aborted."
  exit 1
fi

docker run --rm -v "$REPO_ROOT:/project" -w /project/firmware \
  --device="$PORT" \
  "$IDF_IMAGE" idf.py -p "$PORT" erase-flash

docker run --rm -v "$REPO_ROOT:/project" \
  "$IDF_IMAGE" chown -R "$(id -u):$(id -g)" /project/firmware

echo "Flash erased. Run tools/flash.sh to reflash firmware."
