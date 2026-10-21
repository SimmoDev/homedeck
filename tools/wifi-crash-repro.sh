#!/usr/bin/env bash
# Reboots the device repeatedly through the Web UI until a boot ends in a crash,
# then saves the core dump. Every boot repeats the Wi-Fi connect burst (IP
# acquisition, then mDNS and the Web UI server starting) that the crash tracked
# in https://github.com/SimmoDev/homedeck/issues/8 happens in. The next boot
# after a crash reports its reset reason through GET /api/diagnostics.
#
# Usage: HOMEDECK_PASSWORD=... tools/wifi-crash-repro.sh [host] [max-reboots]
#   host         default homedeck.local (pass an IP if mDNS does not resolve)
#   max-reboots  default 50
#
# Needs curl and jq. Run tools/monitor.sh in another terminal to capture the
# serial log around the fault. Decode the dump with tools/decode-coredump.sh,
# using the ELF of the firmware that is flashed.
set -euo pipefail

for tool in curl jq; do
  command -v "$tool" >/dev/null 2>&1 || { echo "$tool is required." >&2; exit 1; }
done
: "${HOMEDECK_PASSWORD:?set HOMEDECK_PASSWORD to the Web UI admin password}"

HOST="${1:-homedeck.local}"
MAX="${2:-50}"
BASE="http://$HOST"
JAR="$(mktemp)"
trap 'rm -f "$JAR"' EXIT

# The device can answer /api/auth/status before the rest of the Web UI is
# ready, so a request that fails is retried for a while before giving up.
retry() {
  local _
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    "$@" && return 0
    sleep 3
  done
  echo "Gave up on: $*" >&2
  return 1
}

# Sessions live in RAM, so every reboot needs a fresh login.
login() {
  jq -n --arg password "$HOMEDECK_PASSWORD" '{password: $password}' |
    curl -sf --max-time 15 -c "$JAR" -X POST "$BASE/api/auth/login" -H "Content-Type: application/json" -d @- >/dev/null
}

diagnostics_reset_reason() {
  curl -sf --max-time 15 -b "$JAR" "$BASE/api/diagnostics" | jq -r .resetReason
}

wait_for_device() {
  local deadline=$((SECONDS + 120))
  until curl -sf --max-time 3 "$BASE/api/auth/status" >/dev/null 2>&1; do
    [ "$SECONDS" -lt "$deadline" ] || { echo "Device did not come back within 120 s." >&2; return 1; }
    sleep 2
  done
}

# panic, the watchdogs, brownout and sdio all mean the previous boot did not end
# cleanly. software, power-on and external-pin are the expected ones.
unclean() {
  case "$1" in
    software|power-on|external-pin|deep-sleep-wake) return 1 ;;
    *) return 0 ;;
  esac
}

wait_for_device
retry login
echo "Reset reason before starting: $(retry diagnostics_reset_reason)"

for i in $(seq 1 "$MAX"); do
  retry curl -sf --max-time 15 -b "$JAR" -X POST "$BASE/api/ota/reboot" >/dev/null
  sleep 5   # the device restarts 500 ms after the response; let the old server go away
  wait_for_device || exit 2
  sleep 5   # let the boot finish before the next request
  retry login
  reason="$(retry diagnostics_reset_reason)"
  echo "reboot $i/$MAX: previous boot ended with '$reason'"
  if unclean "$reason"; then
    out="coredump-$(date +%Y%m%d-%H%M%S).bin"
    if curl -sf -b "$JAR" -o "$out" "$BASE/api/diagnostics/coredump"; then
      echo "Saved $out ($(wc -c <"$out") bytes). Decode with: tools/decode-coredump.sh $out"
    else
      echo "No core dump was available (reset reason '$reason')."
    fi
    exit 0
  fi
done
echo "No crash in $MAX reboots."
