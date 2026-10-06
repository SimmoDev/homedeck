#!/usr/bin/env bash
# Fails when the simulator's pinned LVGL release differs from the LVGL
# version the firmware build resolves. The firmware gets LVGL
# transitively (espressif/esp_lvgl_port) and firmware/dependencies.lock
# records what it resolved; the simulator pins its own copy with
# FetchContent in simulator/CMakeLists.txt. If they drift, an LVGL API
# deprecation or behaviour change shows up on only one target. Unlike the
# heuristic checks this one is deterministic, so CI treats a mismatch as a
# failure (see lint.yml); run it with no arguments from anywhere in the
# repository.
set -uo pipefail

repo_root="$(git rev-parse --show-toplevel)"
lock="$repo_root/firmware/dependencies.lock"
cmake_file="$repo_root/simulator/CMakeLists.txt"

# `lvgl/lvgl:` is a two-space-indented key under `dependencies:`; its own
# `version:` is the first four-space-indented one after it. The registry
# appends a "~N" revision suffix (9.6.0~1) that is not part of the release.
firmware_version=$(awk '
    /^  lvgl\/lvgl:/ { in_entry = 1; next }
    in_entry && /^  [^ ]/ { exit }
    in_entry && /^    version:/ { sub(/^    version: */, ""); sub(/~.*/, ""); gsub(/["\x27]/, ""); print; exit }
' "$lock")

simulator_version=$(sed -nE 's/^ *GIT_TAG +v?([0-9][0-9.]*) *$/\1/p' "$cmake_file" | head -n 1)

if [ -z "$firmware_version" ] || [ -z "$simulator_version" ]; then
    echo "[lvgl-version-sync] could not read both versions (firmware lock: '${firmware_version}', simulator: '${simulator_version}')"
    exit 1
fi

if [ "$firmware_version" != "$simulator_version" ]; then
    echo "[lvgl-version-sync] firmware resolves LVGL $firmware_version (firmware/dependencies.lock) but the simulator pins $simulator_version (simulator/CMakeLists.txt GIT_TAG) - align them"
    exit 1
fi
