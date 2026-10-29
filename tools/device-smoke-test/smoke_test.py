#!/usr/bin/env python3
"""HomeDeck on-device smoke test driven through the Web UI API (see tools/README.md).

HOMEDECK_PASSWORD=... smoke_test.py [--reboot] [--wait SECONDS] [--min-internal-low-water BYTES]

Checks run in order and each reports PASS, FAIL or SKIP; the exit status is 1 if any failed.
Checks that need a configured Harmony hub or Kodi target skip when none is configured.
--reboot adds a restart through the Web UI and repeats the connection checks afterwards.

HOMEDECK_HOST overrides the device address (default homedeck.local).
"""
import argparse
import http.cookiejar
import json
import os
import sys
import time
import urllib.error
import urllib.request

HOST = os.environ.get("HOMEDECK_HOST", "homedeck.local")
# Resets that mean the previous boot did not end cleanly are everything outside this set. usb and
# jtag are the host resetting the chip, which flashing and debugging do on purpose.
CLEAN_RESETS = {"software", "power-on", "external-pin", "deep-sleep-wake", "usb", "jtag"}
PROBE_MODULE = "smoketest"
PROBE_KEY = "probe"

results = []
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))


def call(path, body=None, timeout=15, authenticated=True, raw=False):
    """Returns (status, parsed JSON or None); never raises on an HTTP error status."""
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(f"http://{HOST}{path}", data=data,
                                     headers={"Content-Type": "application/json"},
                                     method="POST" if body is not None else "GET")
    client = opener if authenticated else urllib.request.build_opener()
    try:
        with client.open(request, timeout=timeout) as response:
            status, payload = response.status, response.read()
    except urllib.error.HTTPError as error:
        status, payload = error.code, error.read()
    if raw:
        return status, payload
    try:
        return status, json.loads(payload or b"{}")
    except ValueError:
        return status, None


def record(name, outcome, detail=""):
    results.append((name, outcome))
    print(f"  {outcome:4}  {name}" + (f" - {detail}" if detail else ""))


def check(name, condition, detail=""):
    record(name, "PASS" if condition else "FAIL", detail)
    return condition


def wait_for(predicate, seconds, interval=2):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError):
            pass
        time.sleep(interval)
    return None


def login():
    status, _ = call("/api/auth/login", {"password": os.environ["HOMEDECK_PASSWORD"]})
    return status == 200


def device_up():
    status, body = call("/api/auth/status", timeout=5, authenticated=False)
    return status == 200 and body is not None


def check_reachable_and_protected():
    print("Reachability and authentication")
    if not wait_for(device_up, 30):
        check("device answers /api/auth/status", False, f"{HOST} did not respond")
        return False
    _, auth = call("/api/auth/status", authenticated=False)
    if not check("a password is set", auth.get("passwordSet") is True):
        return False
    for path in ("/api/settings", "/api/backup", "/api/diagnostics", "/api/harmony/status", "/api/kodi/status"):
        status, _ = call(path, authenticated=False)
        check(f"{path} refuses an unauthenticated request", status == 401, f"status {status}")
    # A wrong-password probe is deliberately absent: the lockout is global, not per client.
    return check("login succeeds", login())


def check_diagnostics(min_low_water):
    print("Diagnostics")
    status, body = call("/api/diagnostics")
    if not check("GET /api/diagnostics", status == 200 and body is not None, f"status {status}"):
        return
    reason = body.get("resetReason")
    check("previous boot ended cleanly", reason in CLEAN_RESETS, f"resetReason={reason}")
    memory = body.get("memory", {})
    low_water = memory.get("internalLowWaterBytes", 0)
    check("internal RAM low-water mark", low_water >= min_low_water,
          f"{low_water} bytes, minimum {min_low_water}")
    check("battery present", body.get("batteryPresent") is True)
    status, ota = call("/api/ota/status")
    version = (ota or {}).get("runningVersion", "")
    # "1" is what ESP-IDF reports when the build was not given a git version.
    check("GET /api/ota/status reports a git version", status == 200 and version not in ("", "1", "unknown"), version)
    status, logs = call("/api/diagnostics/logs", raw=True)
    check("GET /api/diagnostics/logs", status == 200 and len(logs) > 0, f"{len(logs)} bytes")


def check_module(name, path, configured_key, wait_seconds):
    status, body = call(path)
    if status != 200 or body is None:
        check(f"{name} status", False, f"status {status}")
        return
    if not body.get(configured_key):
        record(f"{name} connected", "SKIP", "no target configured")
        return
    connected = wait_for(lambda: (call(path)[1] or {}).get("state") == "connected", wait_seconds)
    check(f"{name} connected", bool(connected), f"within {wait_seconds}s")


def check_modules(wait_seconds):
    print("Module connections")
    check_module("Harmony", "/api/harmony/status", "hasConfig", wait_seconds)
    check_module("Kodi", "/api/kodi/status", "targetConfigured", wait_seconds)


def check_settings():
    print("Settings and backup")
    probe = {"module": PROBE_MODULE, "key": PROBE_KEY, "value": "ok", "schemaVersion": 1}
    status, _ = call("/api/settings", probe)
    check("write a setting", status == 200, f"status {status}")
    _, entries = call("/api/settings")
    stored = [e for e in entries or [] if e.get("module") == PROBE_MODULE and e.get("key") == PROBE_KEY]
    check("setting reads back", len(stored) == 1 and stored[0].get("value") == "ok"
          and stored[0].get("schemaVersion") == 1)
    for version in (4294967296, -1):
        status, body = call("/api/settings", {**probe, "schemaVersion": version})
        check(f"schemaVersion {version} is refused", status == 400 and (body or {}).get("field") == "schemaVersion",
              f"status {status}")
    status, body = call("/api/settings", {**probe, "module": "core", "key": "admin_pw_hash"})
    check("a reserved key is refused", status == 403, f"status {status}")

    status, backup = call("/api/backup")
    in_backup = [e for e in (backup or {}).get("settings", []) if e.get("module") == PROBE_MODULE]
    check("backup contains the setting", status == 200 and len(in_backup) == 1)
    # Entries that are malformed or reserved change nothing, so this restore is safe on a live device.
    status, body = call("/api/backup/restore", {"settings": [
        "not an object",
        {"module": PROBE_MODULE, "key": PROBE_KEY, "value": "x", "schemaVersion": -1},
        {"module": "core", "key": "admin_pw_hash", "value": "x", "schemaVersion": 1}]})
    check("restore skips malformed and rejects reserved entries",
          status == 200 and (body or {}).get("applied") == 0 and body.get("skipped") == 2
          and len(body.get("rejected", [])) == 1, json.dumps(body))
    _, entries = call("/api/settings")
    still = [e for e in entries or [] if e.get("module") == PROBE_MODULE and e.get("key") == PROBE_KEY]
    check("restore left the setting unchanged", len(still) == 1 and still[0].get("value") == "ok")

    status, _ = call("/api/settings/erase", {"module": PROBE_MODULE, "key": PROBE_KEY})
    check("erase the probe setting", status == 200, f"status {status}")
    _, entries = call("/api/settings")
    check("probe setting is gone", not any(e.get("module") == PROBE_MODULE for e in entries or []))


def check_reboot(wait_seconds, min_low_water):
    print("Restart")
    status, _ = call("/api/ota/reboot", {})
    if not check("restart accepted", status == 200, f"status {status}"):
        return
    time.sleep(5)  # the device restarts shortly after the response; let the old server go away
    if not check("device comes back", bool(wait_for(device_up, 600)), "within 600s"):
        return
    check("login after restart", bool(wait_for(login, 60, 3)))
    _, body = call("/api/diagnostics")
    reason = (body or {}).get("resetReason")
    check("restart was a software reset", reason == "software", f"resetReason={reason}")
    check_diagnostics(min_low_water)
    check_modules(wait_seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reboot", action="store_true", help="also restart the device and re-check")
    parser.add_argument("--wait", type=int, default=60, help="seconds to wait for module connections (default 60)")
    parser.add_argument("--min-internal-low-water", type=int, default=51200,
                        help="fail below this internal RAM low-water mark in bytes (default 51200)")
    args = parser.parse_args()
    if "HOMEDECK_PASSWORD" not in os.environ:
        sys.exit("Set HOMEDECK_PASSWORD to the Web UI admin password.")

    print(f"HomeDeck smoke test against {HOST}")
    if check_reachable_and_protected():
        check_diagnostics(args.min_internal_low_water)
        check_modules(args.wait)
        check_settings()
        if args.reboot:
            check_reboot(args.wait, args.min_internal_low_water)

    failed = [name for name, outcome in results if outcome == "FAIL"]
    counts = {o: sum(1 for _, r in results if r == o) for o in ("PASS", "FAIL", "SKIP")}
    print(f"\n{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIP']} skipped")
    for name in failed:
        print(f"  FAILED: {name}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
