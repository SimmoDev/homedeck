#!/usr/bin/env python3
"""Keeps the local Kodi busy for ~50 s with a slow Files.GetDirectory, so a HomeDeck request behind it times out.

Needs slow_source.py running and `pip install websocket-client`. See tools/README.md.
"""
import json, time, websocket
ws = websocket.create_connection("ws://127.0.0.1:9090/jsonrpc", timeout=120)
t0 = time.time()
ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "Files.GetDirectory",
                    "params": {"directory": "http://127.0.0.1:8099/", "media": "video"}}))
print("Kodi is now blocked for ~50 s - open Movies (or TV Shows/Music/Live TV) on the device", flush=True)
while True:
    r = json.loads(ws.recv())
    if r.get("id") == 1:
        print(f"Kodi free again after {time.time()-t0:.0f}s - now leave that screen and open it again", flush=True)
        break
