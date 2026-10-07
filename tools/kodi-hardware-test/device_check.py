#!/usr/bin/env python3
"""HomeDeck on-device Kodi checks driven through the Web UI API (see tools/README.md).

HOMEDECK_PASSWORD=... device_check.py discovery
HOMEDECK_PASSWORD=... device_check.py restart            # stops and starts the local Kodi
HOMEDECK_PASSWORD=... device_check.py monitor [seconds]  # watch the busy flag while tapping through the UI

HOMEDECK_HOST overrides the device address (default homedeck.local).
"""
import http.cookiejar
import json
import os
import signal
import subprocess
import sys
import time
import urllib.request

HOST = os.environ.get("HOMEDECK_HOST", "homedeck.local")
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

def req(path, body=None, timeout=90):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"http://{HOST}{path}", data=data, headers={"Content-Type": "application/json"},
                               method="POST" if body is not None else "GET")
    with op.open(r, timeout=timeout) as resp:
        return json.loads(resp.read() or b"{}")

def login():
    req("/api/auth/login", {"password": os.environ["HOMEDECK_PASSWORD"]})

def status():
    try:
        return req("/api/kodi/status", timeout=10)
    except Exception as e:
        return {"error": str(e)}

def wait(pred, secs, what):
    t0 = time.time()
    while time.time() - t0 < secs:
        s = status()
        if pred(s):
            print(f"  ok: {what} after {time.time()-t0:.0f}s")
            return s
        time.sleep(1)
    print(f"  FAIL: {what} not reached in {secs}s; last={s.get('state', s)}")
    return None

def kodi_pid():
    out = subprocess.run(["pgrep", "-x", "kodi.bin"], capture_output=True, text=True).stdout.split()
    return int(out[0]) if out else None

def discovery():
    s = status()
    print("state:", s.get("state"), "resolvedHost:", s.get("resolvedHost"), "selectedUuid:", s.get("selectedUuid"))
    print("discovered:", s.get("discovered"))
    h = s.get("resolvedHost", "")
    ok = s.get("state") == "connected" and h and not h.startswith("[") and h.replace(".", "").replace(":", "").isdigit()
    print("PASS" if ok else "FAIL", "- connected through an IPv4 address from discovery (manual host override would also show a host)")
    return ok

def restart():
    ok = True
    if not wait(lambda s: s.get("state") == "connected", 30, "connected before restart"):
        return False
    pid = kodi_pid()
    if pid is None:
        print("FAIL: no local kodi.bin process to stop")
        return False
    print("stopping Kodi", pid)
    os.kill(pid, signal.SIGTERM)
    # A second instance would fail to bind 9090/8080, so wait for the first to exit.
    for _ in range(30):
        if kodi_pid() is None:
            break
        time.sleep(1)
    else:
        stuck = kodi_pid()
        if stuck is not None:
            os.kill(stuck, signal.SIGKILL)
        time.sleep(2)
    ok &= wait(lambda s: s.get("state") != "connected", 60, "link reported down") is not None
    time.sleep(5)
    print("starting Kodi")
    subprocess.Popen(["kodi"], env={**os.environ, "DISPLAY": os.environ.get("DISPLAY", ":0")}, stdout=subprocess.DEVNULL,
                     stderr=subprocess.DEVNULL, start_new_session=True)
    s = wait(lambda s: s.get("state") == "connected", 180, "reconnected")
    ok &= s is not None
    if s:
        print("  nowPlaying after reconnect:", s["nowPlaying"]["playback"], repr(s["nowPlaying"]["title"]))
    print("PASS" if ok else "FAIL")
    return ok

def monitor(secs):
    print(f"monitoring {secs}s - open Files > SlowTest on the device now; Ctrl-C to stop early")
    ev, last, t0 = [], None, time.time()
    try:
        while time.time() - t0 < secs:
            s = status()
            cur = (s.get("state"), s.get("libraryBusy"))
            if cur != last:
                ev.append((time.time() - t0, *cur))
                print(f"  t={ev[-1][0]:5.1f}s state={cur[0]} libraryBusy={cur[1]}")
                last = cur
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    busy = [e for e in ev if e[2]]
    saw_busy = bool(busy)
    cleared = saw_busy and not ev[-1][2]
    dropped = any(e[1] != "connected" for e in ev if e[0] > (busy[0][0] if busy else 1e9)) if saw_busy else None
    print("libraryBusy raised:", saw_busy, "| cleared afterwards:", cleared, "| link dropped after it was raised:", dropped)
    print("PASS" if saw_busy and cleared and not dropped else "CHECK (did you open SlowTest and wait for the 30 s message?)")

if __name__ == "__main__":
    login()
    cmd = sys.argv[1] if len(sys.argv) > 1 else "discovery"
    {"discovery": discovery, "restart": restart, "monitor": lambda: monitor(int(sys.argv[2]) if len(sys.argv) > 2 else 120)}[cmd]()
