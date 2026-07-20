#!/usr/bin/env bash
# Builds and flashes HomeDeck firmware onto real Tab5 hardware via the
# pinned espressif/idf Docker image - see DEVELOPMENT.md's ESP-IDF setup
# section for the underlying commands this wraps, including why two
# separate `docker run` calls are needed (the build runs as root inside
# the container; the second call reclaims ownership of what it wrote).
#
# Usage: tools/flash.sh [serial-port]  (default: /dev/ttyACM0)
set -euo pipefail

PORT="${1:-/dev/ttyACM0}"
IDF_IMAGE="espressif/idf:v5.4.3"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

docker run --rm -v "$REPO_ROOT:/project" -w /project/firmware \
  --device="$PORT" \
  "$IDF_IMAGE" idf.py -p "$PORT" set-target esp32p4 build flash

docker run --rm -v "$REPO_ROOT:/project" \
  "$IDF_IMAGE" chown -R "$(id -u):$(id -g)" /project/firmware
