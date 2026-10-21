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

# Sessions live in RAM, so every reboot needs a fresh login.
login() {
  jq -n --arg password "$HOMEDECK_PASSWORD" '{password: $password}' |
    curl -sf -c "$JAR" -X POST "$BASE/api/auth/login" -H "Content-Type: application/json" -d @- >/dev/null
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
login
echo "Reset reason before starting: $(curl -sf -b "$JAR" "$BASE/api/diagnostics" | jq -r .resetReason)"

for i in $(seq 1 "$MAX"); do
  curl -sf -b "$JAR" -X POST "$BASE/api/ota/reboot" >/dev/null
  sleep 5   # the device restarts 500 ms after the response; let the old server go away
  wait_for_device || exit 2
  sleep 5   # let the boot finish before the next request
  login
  reason="$(curl -sf -b "$JAR" "$BASE/api/diagnostics" | jq -r .resetReason)"
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
